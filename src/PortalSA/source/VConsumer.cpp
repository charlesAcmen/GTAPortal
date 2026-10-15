#include "VConsumer.h"
#include <d3d9.h>
#include <windows.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "CameraOverride.h"
#include "StateCapture.h"
#include "../../PortalVisualConfig.h"
#include "../../VToSAFrames.h"

namespace portal_sa::v_consumer { namespace {
struct Vec3 { float x, y, z; };
struct Plane { Vec3 center, right, up; bool placed = false; };
struct UploadSlot {
    IDirect3DTexture9* system = nullptr;
    IDirect3DTexture9* gpu = nullptr;
    IDirect3DQuery9* done = nullptr;
    bool pending = false;
};
HANDLE g_controlMapping = nullptr, g_pixelMapping = nullptr;
v_to_sa::SharedFrames* g_shared = nullptr;
void* g_pixels = nullptr;
std::array<UploadSlot, v_to_sa::kSlotCount> g_uploads{};
IDirect3DDevice9* g_device = nullptr;
IDirect3DStateBlock9* g_savedState = nullptr;
Plane g_plane{};
wchar_t g_planePath[MAX_PATH]{};
LONG g_generation = 0;
int g_active = -1;
bool g_f9Down = false;
bool g_f8Down = false;
bool g_followPlayer = false;
bool g_openOverlayRequested = false;
bool g_publishedLoadedPlane = false;
float g_radius = portal_visual::kRadius;
float g_centerAboveGround = portal_visual::kCenterAboveGround;
float g_savedYawDegrees = portal_visual::gta_sa::yawDegrees;
bool g_depthStateLogged = false;
DWORD g_lastAttachAttempt = 0, g_lastResourceAttempt = 0;
uint64_t g_lastFrame = 0, g_lastReadyTick = 0;

Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 Scale(Vec3 a,float s) { return {a.x*s,a.y*s,a.z*s}; }
Vec3 Cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 Unit(Vec3 a) { const float l=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); return l>0.0001f?Scale(a,1.0f/l):Vec3{}; }
Vec3 Copy(state::Vec3 a) { return {a.x,a.y,a.z}; }

void SetPlaneFromGround(float x,float y,float z,float yawDegrees) {
    constexpr float degreesToRadians=0.01745329251994329577f;
    const float heading=yawDegrees*degreesToRadians;
    const Vec3 forward=Unit({-std::sin(heading),std::cos(heading),0.0f});
    const Vec3 worldUp{0.0f,0.0f,1.0f};
    g_plane.center={x,y,z+g_centerAboveGround};
    g_plane.right=Unit(Cross(forward,worldUp));
    g_plane.up=worldUp;
    g_plane.placed=true;
    g_savedYawDegrees=yawDegrees;
}

void PlanePath() {
    if (g_planePath[0]) return;
    const DWORD length=GetModuleFileNameW(nullptr,g_planePath,MAX_PATH);
    if (!length||length>=MAX_PATH-24) { g_planePath[0]=0; return; }
    wchar_t* slash=std::wcsrchr(g_planePath,L'\\');
    if (!slash) { g_planePath[0]=0; return; }
    wcscpy_s(slash+1,MAX_PATH-(slash+1-g_planePath),L"PortalSAPlane.txt");
}
void SavePlane();
void LoadPlane() {
    PlanePath();
    SetPlaneFromGround(portal_visual::gta_sa::groundX,portal_visual::gta_sa::groundY,
        portal_visual::gta_sa::groundZ,portal_visual::gta_sa::yawDegrees);
    if (!g_planePath[0]) return; FILE* file=nullptr;
    if (_wfopen_s(&file,g_planePath,L"r")||!file){SavePlane();return;}
    char line[512]{};const bool got=std::fgets(line,sizeof(line),file)!=nullptr;std::fclose(file);
    if(!got){SavePlane();return;}
    int version=0;float x=0,y=0,z=0,yaw=0;
    float radius=portal_visual::kRadius,centerAboveGround=portal_visual::kCenterAboveGround;
    if(::sscanf_s(line,"%d %f %f %f %f %f %f",&version,&x,&y,&z,&yaw,
        &radius,&centerAboveGround)==7&&version==3){
        g_radius=std::clamp(radius,0.25f,20.0f);
        g_centerAboveGround=std::clamp(centerAboveGround,-20.0f,20.0f);
        SetPlaneFromGround(x,y,z,yaw);SavePlane();return;
    }
    if(::sscanf_s(line,"%d %f %f %f %f",&version,&x,&y,&z,&yaw)==5&&version==2){
        SetPlaneFromGround(x,y,z,yaw);SavePlane();return;
    }
    Plane legacy{};
    if(::sscanf_s(line,"%f %f %f %f %f %f %f %f %f",
        &legacy.center.x,&legacy.center.y,&legacy.center.z,&legacy.right.x,&legacy.right.y,
        &legacy.right.z,&legacy.up.x,&legacy.up.y,&legacy.up.z)==9){
        const float legacyGroundZ=legacy.center.z-1.10f;
        const float legacyYaw=std::atan2(legacy.right.y,legacy.right.x)*57.295779513082320876f;
        SetPlaneFromGround(legacy.center.x,legacy.center.y,legacyGroundZ,legacyYaw);
        SavePlane();return;
    }
    SavePlane();
}
void SavePlane() {
    PlanePath(); if(!g_planePath[0])return; FILE* file=nullptr;
    if(_wfopen_s(&file,g_planePath,L"w")||!file)return;
    const float groundZ=g_plane.center.z-g_centerAboveGround;
    std::fprintf(file,"3 %.6f %.6f %.6f %.6f %.6f %.6f\n",
        g_plane.center.x,g_plane.center.y,groundZ,g_savedYawDegrees,g_radius,g_centerAboveGround);std::fclose(file);
}
bool IsGameForeground(){DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);return pid==GetCurrentProcessId();}

void ReleaseUploads() {
    if(g_savedState)g_savedState->Release();g_savedState=nullptr;
    for(auto& slot:g_uploads){if(slot.done)slot.done->Release();if(slot.gpu)slot.gpu->Release();
        if(slot.system)slot.system->Release();slot={};}
    if(g_pixels)UnmapViewOfFile(g_pixels); if(g_pixelMapping)CloseHandle(g_pixelMapping);
    g_pixels=nullptr;g_pixelMapping=nullptr;g_generation=0;g_active=-1;
    g_lastFrame=g_lastReadyTick=0;g_device=nullptr;
}
void DetachControl(){ReleaseUploads();if(g_shared)UnmapViewOfFile(g_shared);
    if(g_controlMapping)CloseHandle(g_controlMapping);g_shared=nullptr;g_controlMapping=nullptr;}

bool OwnerAlive() {
    if(!g_shared||!g_shared->ownerPid)return false;
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,g_shared->ownerPid);
    const bool alive=process&&WaitForSingleObject(process,0)==WAIT_TIMEOUT;
    if(process)CloseHandle(process);return alive;
}
bool AttachControl(void(*log)(const char*)) {
    if(g_shared&&OwnerAlive()&&g_shared->magic==v_to_sa::kMagic&&
        g_shared->version==v_to_sa::kVersion)return true;
    if(g_shared)DetachControl(); const DWORD now=GetTickCount();
    if(g_lastAttachAttempt&&now-g_lastAttachAttempt<1000)return false;g_lastAttachAttempt=now;
    g_controlMapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,v_to_sa::kMappingName);
    if(!g_controlMapping)return false;
    g_shared=static_cast<v_to_sa::SharedFrames*>(MapViewOfFile(g_controlMapping,
        FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(v_to_sa::SharedFrames)));
    if(!g_shared||g_shared->magic!=v_to_sa::kMagic||g_shared->version!=v_to_sa::kVersion||
        g_shared->size!=sizeof(*g_shared)||!OwnerAlive()) { DetachControl(); return false; }
    InterlockedExchange(&g_shared->consumerPid,GetCurrentProcessId());
    log("V consumer attached to CPU latest-frame mailbox"); return true;
}

bool ValidLayout() {
    if(!g_shared||!g_shared->generation||!g_shared->width||!g_shared->height||
        g_shared->width>v_to_sa::kMaxDimension||g_shared->height>v_to_sa::kMaxDimension||
        g_shared->format!=v_to_sa::kFormatBgra8)return false;
    const uint64_t row=static_cast<uint64_t>(g_shared->width)*v_to_sa::kBytesPerPixel;
    return g_shared->rowBytes==row&&g_shared->slotBytes==row*g_shared->height&&
        g_shared->pixelMappingName[0];
}
bool CreateUploads(IDirect3DDevice9* device,void(*log)(const char*)) {
    if(!ValidLayout())return false; ReleaseUploads();
    g_pixelMapping=OpenFileMappingW(FILE_MAP_READ,FALSE,g_shared->pixelMappingName);
    if(g_pixelMapping)g_pixels=MapViewOfFile(g_pixelMapping,FILE_MAP_READ,0,0,
        static_cast<SIZE_T>(g_shared->slotBytes*v_to_sa::kSlotCount));
    if(!g_pixels){if(g_pixelMapping)CloseHandle(g_pixelMapping);g_pixelMapping=nullptr;
        InterlockedExchange(&g_shared->lastError,HRESULT_FROM_WIN32(GetLastError()));
        InterlockedExchange(&g_shared->stage,v_to_sa::kMappingError);return false;}
    for(auto& slot:g_uploads){
        HRESULT hr=device->CreateTexture(g_shared->width,g_shared->height,1,0,D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,&slot.system,nullptr);
        if(SUCCEEDED(hr))hr=device->CreateTexture(g_shared->width,g_shared->height,1,0,
            D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&slot.gpu,nullptr);
        if(SUCCEEDED(hr))hr=device->CreateQuery(D3DQUERYTYPE_EVENT,&slot.done);
        if(FAILED(hr)){ReleaseUploads();InterlockedExchange(&g_shared->lastError,hr);
            InterlockedExchange(&g_shared->stage,v_to_sa::kUploadError);log("V consumer upload texture creation failed");return false;}
    }
    g_device=device;g_generation=g_shared->generation;
    InterlockedExchange(&g_shared->lastError,S_OK);log("V consumer native D3D9 upload ring ready");return true;
}

void PollUploads(){for(auto& slot:g_uploads)if(slot.pending){const HRESULT hr=slot.done->GetData(nullptr,0,0);
    if(hr!=S_FALSE)slot.pending=false;}}
int AvailableUpload(){for(int i=0;i<v_to_sa::kSlotCount;++i)if(i!=g_active&&!g_uploads[i].pending)return i;
    for(int i=0;i<v_to_sa::kSlotCount;++i)if(!g_uploads[i].pending)return i;return -1;}
int NewestReady(){int best=-1;for(int i=0;i<v_to_sa::kSlotCount;++i){auto& slot=g_shared->slots[i];
    if(InterlockedCompareExchange(&slot.state,0,0)==v_to_sa::kReady&&(best<0||
        slot.readyTickMs>g_shared->slots[best].readyTickMs||
        (slot.readyTickMs==g_shared->slots[best].readyTickMs&&slot.frameId>g_shared->slots[best].frameId)))best=i;}return best;}

bool UploadNewest(IDirect3DDevice9* device,int uploadIndex) {
    const int mailbox=NewestReady(); if(mailbox<0)return false; auto& metadata=g_shared->slots[mailbox];
    if(metadata.readyTickMs<g_lastReadyTick||(metadata.readyTickMs==g_lastReadyTick&&metadata.frameId<=g_lastFrame))return false;
    if(InterlockedCompareExchange(&metadata.state,v_to_sa::kReading,v_to_sa::kReady)!=v_to_sa::kReady)return false;
    const uint64_t frameId=metadata.frameId,readyTick=metadata.readyTickMs;
    D3DLOCKED_RECT locked{}; HRESULT hr=g_uploads[uploadIndex].system->LockRect(0,&locked,nullptr,0);
    if(SUCCEEDED(hr)){const auto* source=v_to_sa::SlotPixels(g_pixels,g_shared->slotBytes,mailbox);
        for(UINT row=0;row<g_shared->height;++row)std::memcpy(static_cast<uint8_t*>(locked.pBits)+
            static_cast<size_t>(row)*locked.Pitch,source+static_cast<size_t>(row)*g_shared->rowBytes,g_shared->rowBytes);
        g_uploads[uploadIndex].system->UnlockRect(0);}
    InterlockedExchange(&metadata.state,v_to_sa::kFree);
    if(SUCCEEDED(hr))hr=device->UpdateTexture(g_uploads[uploadIndex].system,g_uploads[uploadIndex].gpu);
    if(FAILED(hr)){InterlockedExchange(&g_shared->lastError,hr);InterlockedExchange(&g_shared->stage,v_to_sa::kUploadError);return false;}
    g_active=uploadIndex;g_lastFrame=frameId;g_lastReadyTick=readyTick;return true;
}

HRESULT Draw(IDirect3DDevice9* device,IDirect3DTexture9* texture,void(*log)(const char*)) {
    DWORD sceneDepthMode=D3DZB_TRUE,sceneDepthFunc=D3DCMP_LESSEQUAL;
    DWORD sceneStencil=0,sceneClipPlanes=0,sceneDepthBias=0;
    device->GetRenderState(D3DRS_ZENABLE,&sceneDepthMode);
    device->GetRenderState(D3DRS_ZFUNC,&sceneDepthFunc);
    device->GetRenderState(D3DRS_STENCILENABLE,&sceneStencil);
    device->GetRenderState(D3DRS_CLIPPLANEENABLE,&sceneClipPlanes);
    device->GetRenderState(D3DRS_DEPTHBIAS,&sceneDepthBias);
    IDirect3DSurface9* depth=nullptr;
    if(FAILED(device->GetDepthStencilSurface(&depth))||!depth){
        if(!g_depthStateLogged){log("V consumer draw skipped: no SA scene depth surface is bound");g_depthStateLogged=true;}
        return D3DERR_INVALIDCALL;
    }
    if(!g_depthStateLogged){
        D3DSURFACE_DESC desc{};depth->GetDesc(&desc);
        D3DVIEWPORT9 viewport{};device->GetViewport(&viewport);
        char message[320]{};
        std::snprintf(message,sizeof(message),
            "V consumer scene depth: %ux%u format=%u viewport=(%lu,%lu,%lu,%lu,%.2f,%.2f) z_mode=%lu z_func=%lu stencil=%lu clip_planes=%lu depth_bias=0x%08lX",
            desc.Width,desc.Height,static_cast<unsigned>(desc.Format),
            viewport.X,viewport.Y,viewport.Width,viewport.Height,viewport.MinZ,viewport.MaxZ,
            sceneDepthMode,sceneDepthFunc,sceneStencil,sceneClipPlanes,sceneDepthBias);
        log(message);g_depthStateLogged=true;
    }
    depth->Release();
    if(!g_savedState){
        const HRESULT stateBlock=device->CreateStateBlock(D3DSBT_ALL,&g_savedState);
        if(FAILED(stateBlock))return stateBlock;
    }
    g_savedState->Capture();D3DMATRIX world{};world._11=world._22=world._33=world._44=1;
    D3DMATRIX view{},projection{};std::memcpy(&view,state::g_viewMatrix.data(),sizeof(view));
    std::memcpy(&projection,state::g_projectionMatrix.data(),sizeof(projection));
    device->SetTransform(D3DTS_WORLD,&world);device->SetTransform(D3DTS_VIEW,&view);device->SetTransform(D3DTS_PROJECTION,&projection);
    // Keep the scene's Z/W-buffer convention while removing shadow-stencil and clip masks.
    device->SetRenderState(D3DRS_ZENABLE,sceneDepthMode==D3DZB_USEW?D3DZB_USEW:D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    const bool sceneUsesOrderedDepth=sceneDepthFunc==D3DCMP_LESS||
        sceneDepthFunc==D3DCMP_LESSEQUAL||sceneDepthFunc==D3DCMP_GREATER||
        sceneDepthFunc==D3DCMP_GREATEREQUAL;
    device->SetRenderState(D3DRS_ZFUNC,sceneUsesOrderedDepth?
        sceneDepthFunc:D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_STENCILENABLE,FALSE);
    device->SetRenderState(D3DRS_CLIPPLANEENABLE,0);
    device->SetRenderState(D3DRS_DEPTHBIAS,0);
    device->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,0);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID);device->SetRenderState(D3DRS_LIGHTING,FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    device->SetRenderState(D3DRS_FOGENABLE,FALSE);device->SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE|D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE);device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
    device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_DISABLE);device->SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE);
    device->SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0);device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
    device->SetVertexShader(nullptr);device->SetPixelShader(nullptr);device->SetFVF(D3DFVF_XYZ|D3DFVF_TEX1);device->SetTexture(0,texture);
    struct Vertex{float x,y,z,u,v;};
    constexpr int segments=48;
    std::array<Vertex,segments+2> vertices{};
    const float centerU=(portal_visual::kCropLeft+portal_visual::kCropRight)*0.5f;
    const float centerV=(portal_visual::kCropTop+portal_visual::kCropBottom)*0.5f;
    vertices[0]={g_plane.center.x,g_plane.center.y,g_plane.center.z,centerU,centerV};
    // Visual-only billboard uses the vector from the portal centre to this
    // game's real camera position. Camera look direction and CJ are irrelevant.
    const Vec3 camera=Copy(state::g_state.cameraPosition);
    constexpr float pi=3.14159265358979323846f;
    const float facingYaw=std::atan2(-(camera.x-g_plane.center.x),
                                     camera.y-g_plane.center.y)+pi;
    const Vec3 runtimeRight{std::cos(facingYaw),std::sin(facingYaw),0.0f};
    for(int segment=0;segment<=segments;++segment){
        const float angle=2.0f*pi*static_cast<float>(segment)/segments;
        const float nx=std::cos(angle),ny=std::sin(angle);
        const float x=nx*g_radius;
        const float y=ny*g_radius;
        const Vec3 position=Add(Add(g_plane.center,Scale(runtimeRight,x)),Scale(g_plane.up,y));
        const float u=portal_visual::kCropLeft+(nx*0.5f+0.5f)*
            (portal_visual::kCropRight-portal_visual::kCropLeft);
        const float v=portal_visual::kCropTop+(0.5f-ny*0.5f)*
            (portal_visual::kCropBottom-portal_visual::kCropTop);
        vertices[segment+1]={position.x,position.y,position.z,u,v};
    }
    const HRESULT drawn=device->DrawPrimitiveUP(D3DPT_TRIANGLEFAN,
        segments,vertices.data(),sizeof(Vertex));
    g_savedState->Apply();return drawn;
}
} // namespace

void OnGameProcess(void(*log)(const char*)) {
    static bool loaded=false;if(!loaded){LoadPlane();loaded=true;}
    const bool foreground=IsGameForeground();
    const bool editPressed=(GetAsyncKeyState(VK_F8)&0x8000)!=0;
    if(editPressed&&!g_f8Down&&foreground){
        if(g_followPlayer){
            g_followPlayer=false;
            SavePlane();
            log("SA portal follow stopped by F8; placement locked for size editing");
        }
        g_openOverlayRequested=true;
    }
    g_f8Down=editPressed;
    const bool pressed=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
    if(pressed&&!g_f9Down&&foreground){
        g_followPlayer=!g_followPlayer;
        g_openOverlayRequested=true;
        if(!g_followPlayer){SavePlane();log("SA portal follow stopped; current placement saved");}
        else log("SA portal follow started; press F9 again to lock and save");
    }
    if(g_followPlayer&&state::g_state.playerValid&&state::g_state.cameraValid){
        const float heading=state::g_state.playerHeading;
        const Vec3 forward=Unit({-std::sin(heading),std::cos(heading),0.0f});
        const Vec3 worldUp{0.0f,0.0f,1.0f},right=Unit(Cross(forward,worldUp));
        if(right.x||right.y||right.z){
            const Vec3 player=Copy(state::g_state.playerPosition);
            g_plane.center=player;
            g_plane.center.z=player.z+g_centerAboveGround;
            g_plane.right=right;g_plane.up=worldUp;g_plane.placed=true;
            constexpr float radiansToDegrees=57.295779513082320876f;
            g_savedYawDegrees=heading*radiansToDegrees;
            camera_override::PublishPortalPlacement(
                g_plane.center.x,g_plane.center.y,g_plane.center.z,g_savedYawDegrees);
            g_depthStateLogged=false;
        }
    }
    if(!g_publishedLoadedPlane&&g_plane.placed){
        camera_override::PublishPortalPlacement(
            g_plane.center.x,g_plane.center.y,g_plane.center.z,g_savedYawDegrees);
        g_publishedLoadedPlane=true;
    }
    g_f9Down=pressed;
}

void OnRenderScene(IDirect3DDevice9* device,void(*log)(const char*)) {
    if(!device||!camera_override::PortalVisible()||!AttachControl(log))return;
    if(g_generation!=g_shared->generation||g_device!=device||!g_pixels){const DWORD now=GetTickCount();
        if(g_lastResourceAttempt&&now-g_lastResourceAttempt<500)return;g_lastResourceAttempt=now;
        if(!CreateUploads(device,log))return;}
    PollUploads();const int available=AvailableUpload();if(available>=0)UploadNewest(device,available);
    if(g_active<0||!g_plane.placed||!state::g_state.cameraValid||GetTickCount64()-g_lastReadyTick>500)return;
    const HRESULT drawn=Draw(device,g_uploads[g_active].gpu,log);
    if(FAILED(drawn)){InterlockedExchange(&g_shared->lastError,drawn);InterlockedExchange(&g_shared->stage,v_to_sa::kDrawError);return;}
    if(SUCCEEDED(g_uploads[g_active].done->Issue(D3DISSUE_END))){g_uploads[g_active].pending=true;
        InterlockedIncrement(&g_shared->displayed);InterlockedExchange(&g_shared->stage,v_to_sa::kStreaming);}
}
void BeforeReset(){ReleaseUploads();}
VisualTuning GetVisualTuning(){return {g_radius,g_centerAboveGround,g_followPlayer};}
void SetVisualTuning(float radius,float centerAboveGround){
    radius=std::clamp(radius,0.25f,20.0f);
    centerAboveGround=std::clamp(centerAboveGround,-20.0f,20.0f);
    const float groundZ=g_plane.center.z-g_centerAboveGround;
    g_radius=radius;g_centerAboveGround=centerAboveGround;g_plane.center.z=groundZ+g_centerAboveGround;
    g_depthStateLogged=false;
}
void SaveVisualTuning(){SavePlane();}
bool ConsumeOverlayOpenRequest(){const bool requested=g_openOverlayRequested;g_openOverlayRequested=false;return requested;}
} // namespace portal_sa::v_consumer
