#include "VConsumer.h"
#include <d3d9.h>
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "StateCapture.h"
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
Plane g_plane{};
wchar_t g_planePath[MAX_PATH]{};
LONG g_generation = 0;
int g_active = -1;
bool g_f9Down = false;
bool g_depthStateLogged = false;
DWORD g_lastAttachAttempt = 0, g_lastResourceAttempt = 0;
uint64_t g_lastFrame = 0, g_lastReadyTick = 0;

Vec3 Add(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Vec3 Scale(Vec3 a,float s) { return {a.x*s,a.y*s,a.z*s}; }
Vec3 Cross(Vec3 a,Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
Vec3 Unit(Vec3 a) { const float l=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z); return l>0.0001f?Scale(a,1.0f/l):Vec3{}; }
Vec3 Copy(state::Vec3 a) { return {a.x,a.y,a.z}; }

void PlanePath() {
    if (g_planePath[0]) return;
    const DWORD length=GetModuleFileNameW(nullptr,g_planePath,MAX_PATH);
    if (!length||length>=MAX_PATH-24) { g_planePath[0]=0; return; }
    wchar_t* slash=std::wcsrchr(g_planePath,L'\\');
    if (!slash) { g_planePath[0]=0; return; }
    wcscpy_s(slash+1,MAX_PATH-(slash+1-g_planePath),L"PortalSAPlane.txt");
}
void LoadPlane() {
    PlanePath(); if (!g_planePath[0]) return; FILE* file=nullptr;
    if (_wfopen_s(&file,g_planePath,L"r")||!file) return; Plane value{};
    const int read=::fscanf_s(file,"%f %f %f %f %f %f %f %f %f",
        &value.center.x,&value.center.y,&value.center.z,&value.right.x,&value.right.y,
        &value.right.z,&value.up.x,&value.up.y,&value.up.z);
    std::fclose(file); if(read==9){value.placed=true;g_plane=value;}
}
void SavePlane() {
    PlanePath(); if(!g_planePath[0])return; FILE* file=nullptr;
    if(_wfopen_s(&file,g_planePath,L"w")||!file)return;
    std::fprintf(file,"%.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n",
        g_plane.center.x,g_plane.center.y,g_plane.center.z,g_plane.right.x,g_plane.right.y,
        g_plane.right.z,g_plane.up.x,g_plane.up.y,g_plane.up.z); std::fclose(file);
}
bool IsGameForeground(){DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);return pid==GetCurrentProcessId();}

void ReleaseUploads() {
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
    IDirect3DStateBlock9* saved=nullptr;const HRESULT stateBlock=device->CreateStateBlock(D3DSBT_ALL,&saved);
    if(FAILED(stateBlock))return stateBlock;saved->Capture();D3DMATRIX world{};world._11=world._22=world._33=world._44=1;
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
    struct Vertex{float x,y,z,u,v;};const float aspect=g_shared&&g_shared->width?
        static_cast<float>(g_shared->height)/g_shared->width:9.0f/16.0f;
    const Vec3 right=Scale(g_plane.right,1.2f),up=Scale(g_plane.up,1.2f*aspect);
    const Vec3 a=Add(Add(g_plane.center,Scale(right,-1)),up),b=Add(Add(g_plane.center,right),up);
    const Vec3 c=Add(Add(g_plane.center,Scale(right,-1)),Scale(up,-1)),d=Add(Add(g_plane.center,right),Scale(up,-1));
    const Vertex vertices[]={{a.x,a.y,a.z,0,0},{b.x,b.y,b.z,1,0},{c.x,c.y,c.z,0,1},{d.x,d.y,d.z,1,1}};
    const HRESULT drawn=device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex));
    saved->Apply();saved->Release();return drawn;
}
} // namespace

void OnGameProcess(void(*log)(const char*)) {
    static bool loaded=false;if(!loaded){LoadPlane();loaded=true;}
    const bool pressed=(GetAsyncKeyState(VK_F9)&0x8000)!=0;
    if(pressed&&!g_f9Down&&IsGameForeground()&&state::g_state.playerValid&&state::g_state.cameraValid){
        const float heading=state::g_state.playerHeading;
        const Vec3 forward=Unit({-std::sin(heading),std::cos(heading),0.0f});
        const Vec3 worldUp{0.0f,0.0f,1.0f},right=Unit(Cross(forward,worldUp));
        if(right.x||right.y||right.z){
            // Anchor the pane in front of CJ, not in front of the third-person camera.
            // The camera is already about three metres behind CJ in the normal view.
            const Vec3 player=Copy(state::g_state.playerPosition);
            g_plane.center=Add(player,Scale(forward,3.0f));
            g_plane.center.z=player.z+1.5f;
            g_plane.right=right;g_plane.up=worldUp;g_plane.placed=true;SavePlane();
            g_depthStateLogged=false;
            char message[256]{};
            std::snprintf(message,sizeof(message),
                "V consumer portal placed 3 m ahead of player: center=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f) up=(0,0,1)",
                g_plane.center.x,g_plane.center.y,g_plane.center.z,
                g_plane.right.x,g_plane.right.y,g_plane.right.z);
            log(message);
        }
    }
    g_f9Down=pressed;
}

void OnRenderScene(IDirect3DDevice9* device,void(*log)(const char*)) {
    if(!device||!AttachControl(log))return;
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
} // namespace portal_sa::v_consumer
