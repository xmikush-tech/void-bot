// ============================================================================
//  main_loader.cpp — VOIDLoader  (Win32 + DX11 + ImGui)
//  BLACK HOLE centrato — logo buco nero grande al centro, PIN sotto
//  Niente glow angoli. Tutto animato.
// ============================================================================
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <winhttp.h>
#include <iphlpapi.h>
#include <sstream>
#include <atomic>
#include <thread>

#include "gui/imgui/imgui.h"
#include "gui/imgui/imgui_impl_win32.h"
#include "gui/imgui/imgui_impl_dx11.h"
#include "pfp_data.h"

static constexpr int         PIN_LEN     = 6;
static constexpr const char* CHEAT_EXE  = "void.exe";
static constexpr const wchar_t* API_HOST = L"void-bot-production-260b.up.railway.app";
static constexpr const wchar_t* API_PATH = L"/auth/pin";

// ── Download URL for void.exe (the actual cheat payload) ─────────────────────
// Change v1.0 if you bump the release tag
static constexpr const wchar_t* DL_HOST = L"github.com";
static constexpr const wchar_t* DL_PATH = L"/xmikush-tech/void-bot/releases/download/v1.0/void.exe";

// ── Raccoglie HWID (primo MAC address fisico) ─────────────────────────────
static std::string get_hwid(){
    IP_ADAPTER_INFO buf[16]; ULONG sz=sizeof(buf);
    if(GetAdaptersInfo(buf,&sz)!=NO_ERROR) return "UNKNOWN";
    char tmp[64];
    auto& a=buf[0].Address;
    sprintf_s(tmp,"%02X-%02X-%02X-%02X-%02X-%02X",
              a[0],a[1],a[2],a[3],a[4],a[5]);
    return tmp;
}

// ── Chiamata POST /auth/pin → {valid, product, discord_id} ───────────────
// Ritorna 0=errore rete, 1=PIN valido, 2=PIN non valido/scaduto
static int api_validate_pin(const std::string& pin,
                             std::string& out_product,
                             std::string& out_msg){
    std::string hwid = get_hwid();
    // body JSON
    std::string body = "{\"pin\":\"" + pin + "\",\"hwid\":\"" + hwid + "\"}";

    HINTERNET hSess = WinHttpOpen(L"VOIDLoader/1.0",
                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if(!hSess) return 0;

    HINTERNET hConn = WinHttpConnect(hSess, API_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if(!hConn){ WinHttpCloseHandle(hSess); return 0; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"POST", API_PATH,
                        nullptr, WINHTTP_NO_REFERER,
                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                        WINHTTP_FLAG_SECURE);
    if(!hReq){ WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return 0; }

    const wchar_t* hdrs = L"Content-Type: application/json";
    BOOL ok = WinHttpSendRequest(hReq, hdrs, (DWORD)-1,
                  (LPVOID)body.c_str(), (DWORD)body.size(),
                  (DWORD)body.size(), 0);
    if(!ok || !WinHttpReceiveResponse(hReq, nullptr)){
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSess);
        return 0;
    }

    // leggi risposta
    std::string resp;
    DWORD avail=0;
    while(WinHttpQueryDataAvailable(hReq,&avail) && avail>0){
        std::vector<char> tmp(avail+1,0);
        DWORD read=0;
        WinHttpReadData(hReq,tmp.data(),avail,&read);
        resp.append(tmp.data(),read);
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSess);

    // parse JSON minimale (no dipendenze esterne)
    auto jval=[&](const std::string& key)->std::string{
        std::string pat="\""+key+"\":\"";
        auto p=resp.find(pat); if(p==std::string::npos) return "";
        p+=pat.size();
        if(resp[p]=='"'){
            p++; auto e=resp.find('"',p);
            return e==std::string::npos?"":resp.substr(p,e-p);
        }
        // bool/number
        auto e=resp.find_first_of(",}",p);
        return e==std::string::npos?resp.substr(p):resp.substr(p,e-p);
    };

    bool valid = jval("valid")=="true";
    out_product = jval("product");
    out_msg     = jval("message");
    return valid ? 1 : 2;
}

// ── Download void.exe via WinHTTP (follows GitHub CDN redirects) ──────────
// Returns true on success. Saves to dest path.
static bool download_cheat(const std::string& dest){
    HINTERNET hSess = WinHttpOpen(L"VOIDLoader/1.0",
                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if(!hSess) return false;

    HINTERNET hConn = WinHttpConnect(hSess, DL_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if(!hConn){ WinHttpCloseHandle(hSess); return false; }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", DL_PATH,
                        nullptr, WINHTTP_NO_REFERER,
                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                        WINHTTP_FLAG_SECURE);
    if(!hReq){ WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return false; }

    // follow redirects — GitHub releases 302 → objects.githubusercontent.com
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hReq, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    if(!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
       !WinHttpReceiveResponse(hReq, nullptr)){
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
        return false;
    }

    FILE* f = fopen(dest.c_str(), "wb");
    if(!f){ WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess); return false; }

    bool ok = true;
    DWORD avail = 0, read = 0;
    std::vector<char> buf(65536);
    while(WinHttpQueryDataAvailable(hReq, &avail) && avail > 0){
        DWORD chunk = (avail < (DWORD)buf.size()) ? avail : (DWORD)buf.size();
        if(!WinHttpReadData(hReq, buf.data(), chunk, &read) || read == 0){ ok = false; break; }
        if(fwrite(buf.data(), 1, read, f) != read){ ok = false; break; }
    }
    fclose(f);
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSess);
    return ok;
}

#define GRY(a)  IM_COL32(210,210,210,a)
#define GRY2(a) IM_COL32(150,150,150,a)
#define GRY3(a) IM_COL32( 90, 90, 90,a)
#define WHT(a)  IM_COL32(245,245,245,a)
#define BG(a)   IM_COL32(  6,  6,  6,a)
#define BRD(a)  IM_COL32( 40, 40, 40,a)
#define DIM(a)  IM_COL32( 70, 70, 70,a)
#define ERR(a)  IM_COL32(220, 60, 50,a)
#define GLW(a)  IM_COL32(255,255,255,a)

static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dCtx    = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_pRTV       = nullptr;
static HWND                     g_hWnd       = nullptr;
static int                      g_W = 680, g_H = 480;
static ID3D11ShaderResourceView* g_pfpSRV    = nullptr;  // pfp texture

// ── Carica PNG da memoria (embedded) via WIC stream → SRV ─────────────────────
static bool load_texture_from_memory(const unsigned char* data, int data_sz,
                                     ID3D11Device* dev,
                                     ID3D11ShaderResourceView** out_srv){
    if(!dev||!out_srv) return false;
    IWICImagingFactory*    wic  =nullptr;
    IWICStream*            stm  =nullptr;
    IWICBitmapDecoder*     dec  =nullptr;
    IWICBitmapFrameDecode* frame=nullptr;
    IWICFormatConverter*   conv =nullptr;
    bool ok=false;

    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,
        CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)))) return false;
    if(FAILED(wic->CreateStream(&stm))) goto cleanup;
    if(FAILED(stm->InitializeFromMemory((BYTE*)data,(DWORD)data_sz))) goto cleanup;
    if(FAILED(wic->CreateDecoderFromStream(stm,nullptr,
        WICDecodeMetadataCacheOnLoad,&dec))) goto cleanup;
    if(FAILED(dec->GetFrame(0,&frame))) goto cleanup;
    if(FAILED(wic->CreateFormatConverter(&conv))) goto cleanup;
    if(FAILED(conv->Initialize(frame,GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,nullptr,0.f,WICBitmapPaletteTypeCustom))) goto cleanup;
    {
        UINT W=0,H=0; conv->GetSize(&W,&H);
        std::vector<BYTE> buf(W*H*4);
        conv->CopyPixels(nullptr,W*4,(UINT)buf.size(),buf.data());
        D3D11_TEXTURE2D_DESC td{};
        td.Width=W;td.Height=H;td.MipLevels=1;td.ArraySize=1;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd2{};
        sd2.pSysMem=buf.data();sd2.SysMemPitch=W*4;
        ID3D11Texture2D* tex=nullptr;
        if(SUCCEEDED(dev->CreateTexture2D(&td,&sd2,&tex))){
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format=td.Format;
            srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels=1;
            dev->CreateShaderResourceView(tex,&srv,out_srv);
            tex->Release(); ok=true;
        }
    }
cleanup:
    if(conv) conv->Release();
    if(frame)frame->Release();
    if(dec)  dec->Release();
    if(stm)  stm->Release();
    if(wic)  wic->Release();
    return ok;
}

// ── Particelle ────────────────────────────────────────────────────────────────
struct Particle { float x,y,vy,vx,size,life,maxlife,flicker; int type; };
static constexpr int NUM_P=70;
static Particle g_parts[NUM_P];
static bool     g_parts_init=false;

static float frand(float lo,float hi){return lo+(hi-lo)*((float)(rand()&0x7FFF)/0x7FFF);}

static void reset_p(Particle& p,bool bot=true){
    p.x=frand(0.f,(float)g_W);
    p.y=bot?frand((float)g_H,(float)g_H+100.f):frand(0.f,(float)g_H);
    p.vy=frand(-32.f,-7.f);
    p.vx=frand(-2.f,2.f);
    p.size=frand(0.7f,3.f);
    p.maxlife=frand(3.f,12.f);
    p.life=bot?0.f:frand(0.f,p.maxlife);
    p.flicker=frand(0.f,6.28f);
    p.type=rand()%4; // 0=dot 1=cross 2=spark 3=ring
}
static void init_particles(){srand((unsigned)GetTickCount()); for(auto& p:g_parts)reset_p(p,false); g_parts_init=true;}
static void update_particles(float dt){
    for(auto& p:g_parts){
        p.life+=dt; p.y+=p.vy*dt; p.x+=p.vx*dt;
        p.flicker+=dt*frand(1.f,4.5f);
        if(p.life>=p.maxlife||p.y<-15.f) reset_p(p,true);
    }
}
// zone_x0..zone_x1, zone_y0..zone_y1 = area da NON disegnare (PIN boxes)
static void draw_particles(ImDrawList* dl,float ga,
    float zone_x0=0.f,float zone_y0=0.f,float zone_x1=0.f,float zone_y1=0.f){
    bool has_zone=(zone_x1>zone_x0 && zone_y1>zone_y0);
    for(auto& p:g_parts){
        // skip se la particella è nella zona protetta PIN (con margine 10px)
        if(has_zone){
            float mx=10.f,my=8.f;
            if(p.x>=zone_x0-mx&&p.x<=zone_x1+mx&&p.y>=zone_y0-my&&p.y<=zone_y1+my)
                continue;
        }
        float lt=p.life/p.maxlife;
        float fa=lt<0.12f?lt/0.12f:(lt>0.78f?(1.f-lt)/0.22f:1.f);
        float flk=0.35f+0.65f*sinf(p.flicker*3.8f);
        float aa=fa*flk*ga; if(aa<0.01f) continue;
        int al=(int)(aa*115.f);
        ImU32 col=IM_COL32(195,195,195,al);
        switch(p.type){
        case 0: dl->AddCircleFilled({p.x,p.y},p.size,col,8); break;
        case 1:{float s=p.size*1.3f;dl->AddLine({p.x-s,p.y},{p.x+s,p.y},col,.7f);dl->AddLine({p.x,p.y-s},{p.x,p.y+s},col,.7f);}break;
        case 2: dl->AddLine({p.x,p.y},{p.x+p.vx*0.4f,p.y-p.size*4.f},col,.55f); break;
        case 3: dl->AddCircle({p.x,p.y},p.size*1.5f,col,12,.6f); break;
        }
    }
}

// ── State ─────────────────────────────────────────────────────────────────────
namespace st{
    char  boxes[PIN_LEN][2]={};
    int   focused=0;
    bool  wrong=false;
    float shake_t=0.f;
    float alpha=0.f;
    float time=0.f;
    bool  authed=false;
    bool  dashboard=false;   // mostra dashboard dopo PIN
    bool  loading=false;     // avviato inject (Play premuto)
    float progress=0.f;
    float pulse=0.f;
    float orbit_a=0.f;
    float orbit2_a=1.8f;
    float orbit3_a=3.5f;
    float accretion=0.f;
    float jet_t=0.f;
    float dash_alpha=0.f;    // fade-in dashboard
    const char* phases[]={"Authenticating...","Loading modules...","Connecting...","Finalizing...","Ready."};
}

static void launch_cheat(){
    char dir[MAX_PATH]{};GetModuleFileNameA(nullptr,dir,MAX_PATH);
    if(char* s=strrchr(dir,'\\'))s[1]='\0';
    std::string path=std::string(dir)+CHEAT_EXE;
    SHELLEXECUTEINFOA sei{sizeof(sei)};sei.fMask=SEE_MASK_NO_CONSOLE;
    sei.lpVerb="runas";sei.lpFile=path.c_str();sei.nShow=SW_HIDE;
    ShellExecuteExA(&sei);PostQuitMessage(0);
}
static bool pin_full(){for(int i=0;i<PIN_LEN;i++)if(!st::boxes[i][0])return false;return true;}
static std::string collect_pin(){std::string s;for(int i=0;i<PIN_LEN;i++)s+=st::boxes[i][0];return s;}
static void clear_boxes(){for(int i=0;i<PIN_LEN;i++)st::boxes[i][0]='\0';st::focused=0;}

// stato auth asincrono
static std::atomic<int>  g_auth_result{0}; // 0=idle,1=checking,2=ok,3=fail,4=neterr
static std::string       g_auth_product;

// stato download asincrono
static std::atomic<bool> g_dl_done{false};    // true = download completato con successo
static std::atomic<bool> g_dl_started{false}; // true = thread già lanciato

static void try_submit(){
    if(!pin_full()){st::wrong=true;st::shake_t=1.f;return;}
    if(g_auth_result==1) return; // già in corso
    std::string pin=collect_pin();
    g_auth_result=1; // checking
    // thread per non bloccare il render
    std::thread([pin](){
        std::string product,msg;
        int r=api_validate_pin(pin,product,msg);
        g_auth_product=product;
        g_auth_result=(r==1)?2:(r==2)?3:4;
    }).detach();
}

static ImFont* g_fBody =nullptr;
static ImFont* g_fTitle=nullptr;
static ImFont* g_fSmall=nullptr;
static ImFont* g_fBrand=nullptr;

static void TextCX(ImDrawList* dl,ImFont* f,float sz,float y,float x0,float x1,ImU32 col,const char* txt){
    float tw=f->CalcTextSizeA(sz,FLT_MAX,0.f,txt).x;
    dl->AddText(f,sz,{x0+(x1-x0-tw)*.5f,y},col,txt);
}

// ── BUCO NERO — disco OBLIQUO stile Interstellar ──────────────────────────────
// Disco inclinato ~30° rispetto all'orizzontale — non verticale, non simmetrico
// cx,cy = centro   R = raggio fotosfera   a = alpha globale
static void draw_black_hole(ImDrawList* dl,float cx,float cy,float R,float a){
    const float PI=3.14159265f;
    // angolo di inclinazione del piano del disco (radianti)
    // ~30° = 0.524 rad — come Gargantua in Interstellar
    const float TILT=0.524f;  // inclinazione fissa del piano
    const float CT=cosf(TILT), ST2=sinf(TILT); // proiezione X,Y sul piano inclinato

    // Helper: proietta un punto sull'ellisse obliqua ruotata
    // ang = angolo orbitale,  rx = semiasse X disco,  ry = semiasse Y schiacciato
    // Il piano è inclinato: X = rx*cos(ang)*cos(tilt) - ry*sin(ang)*sin(tilt)
    //                       Y = rx*cos(ang)*sin(tilt) + ry*sin(ang)*cos(tilt)
    // con ry piccolo (<<rx) = disco quasi piatto visto da angolo basso
    auto disk_pt=[&](float ang,float rx,float ry)->ImVec2{
        float lx=rx*cosf(ang);
        float ly=ry*sinf(ang);
        return {cx + lx*CT - ly*ST2,
                cy + lx*ST2+ ly*CT};
    };

    // --- GLOW DIFFUSO — ellisse obliqua prima di tutto (dietro) ---
    for(int g2=6;g2>=0;g2--){
        float gr=R*(1.4f+g2*0.22f);
        float gry=gr*0.20f;
        float glo=(0.055f-g2*0.007f)*a; if(glo<=0.f)continue;
        const int SEG=52;
        ImVec2 pts[SEG];
        for(int si=0;si<SEG;si++){
            float ang2=(float)si/(float)SEG*2.f*PI;
            pts[si]=disk_pt(ang2,gr,gry);
        }
        dl->AddConvexPolyFilled(pts,SEG,GLW((int)(glo*255.f)));
    }

    // --- DISCO DI ACCRESCIMENTO — 7 strati obliqui, stile Interstellar ---
    // parte INFERIORE del disco (davanti all'orizzonte — disegnata DOPO il buco nero)
    // parte SUPERIORE (dietro l'orizzonte — disegnata ORA prima del cerchio nero)
    for(int pass=0;pass<2;pass++){
        // pass 0 = arco superiore (va dietro), pass 1 = arco inferiore (va davanti)
        // l'arco "superiore" visivo = angoli dove Y proiettato è negativo (sopra centro)
        for(int layer=0;layer<7;layer++){
            float rx   = R*(1.15f+layer*0.20f);
            float ry   = rx*0.16f;
            float thick= R*(0.065f-layer*0.008f); if(thick<R*0.010f)thick=R*0.010f;
            float rot  = st::accretion*(0.9f+layer*0.12f)+layer*0.28f;
            float bright=(0.92f-layer*0.11f)*a; if(bright<0.f)break;
            int   gc   =(int)(bright*248.f);
            ImU32 col  =IM_COL32(gc,gc,gc,(int)(bright*255.f));

            int segs=80;
            ImVec2 pts[84];
            float gap=0.05f*PI; // gap piccolo per arco quasi completo
            float span=PI-gap;  // solo mezzo giro per arco

            if(pass==0){
                // arco superiore: da PI a 2PI (la parte "in cima" = Y negativa sul piano inclinato)
                for(int i=0;i<segs;i++){
                    float ang=rot+PI+gap*.5f+span*(float)i/(float)(segs-1);
                    pts[i]=disk_pt(ang,rx,ry);
                }
            } else {
                // arco inferiore: da 0 a PI (la parte "in fondo" = Y positiva)
                for(int i=0;i<segs;i++){
                    float ang=rot+gap*.5f+span*(float)i/(float)(segs-1);
                    pts[i]=disk_pt(ang,rx,ry);
                }
            }
            if(pass==0) dl->AddPolyline(pts,segs,col,false,thick);
            // pass==1 disegnato dopo il buco nero (vedi sotto)
            // salviamo in un lambda locale — usiamo array statici per i 2 pass
        }
    }

    // (filamenti rimossi — causavano punti visibili ai bordi del disco)

    // --- GLOW SFERICO CENTRALE (alone attorno all'orizzonte) ---
    for(int g2=0;g2<5;g2++){
        float gr=R*(0.98f+g2*0.20f);
        float glo=(0.040f-g2*0.008f)*a; if(glo<=0.f)break;
        dl->AddCircleFilled({cx,cy},gr,GLW((int)(glo*255.f)),48);
    }

    // --- FOTOSFERA / EINSTEIN RING ---
    {
        float pr=R*0.80f;
        float pw=R*0.052f;
        float pb=(0.72f+0.20f*sinf(st::time*2.2f))*a;
        dl->AddCircleFilled({cx,cy},pr+pw,GLW((int)(pb*255.f)),64);
        dl->AddCircle({cx,cy},pr+pw,GRY((int)(pb*110.f)),64,pw*0.35f);
        dl->AddCircleFilled({cx,cy},pr-pw*0.18f,BG(255),64);
    }

    // --- ORIZZONTE DEGLI EVENTI (disco nero) ---
    dl->AddCircleFilled({cx,cy},R*0.64f,BG(255),64);

    // --- ANELLO INTERNO BRILLANTE ---
    {
        float ir=R*0.66f;
        float ib=(0.38f+0.20f*sinf(st::time*2.8f+0.7f))*a;
        dl->AddCircle({cx,cy},ir,GLW((int)(ib*255.f)),64,R*0.026f);
    }

    // --- ARCO INFERIORE DEL DISCO (davanti all'event horizon) ---
    for(int layer=0;layer<7;layer++){
        float rx   = R*(1.15f+layer*0.20f);
        float ry   = rx*0.16f;
        float thick= R*(0.065f-layer*0.008f); if(thick<R*0.010f)thick=R*0.010f;
        float rot  = st::accretion*(0.9f+layer*0.12f)+layer*0.28f;
        float bright=(0.92f-layer*0.11f)*a; if(bright<0.f)break;
        int   gc   =(int)(bright*248.f);
        ImU32 col  =IM_COL32(gc,gc,gc,(int)(bright*255.f));

        int segs=80;
        ImVec2 pts[84];
        float gap=0.05f*PI;
        float span=PI-gap;
        for(int i=0;i<segs;i++){
            float ang=rot+gap*.5f+span*(float)i/(float)(segs-1);
            pts[i]=disk_pt(ang,rx,ry);
        }
        dl->AddPolyline(pts,segs,col,false,thick);
    }

    // --- PARTICELLE ORBITALI — seguono il piano inclinato ---
    // guard: disegna solo se la particella è dentro il disco visibile
    // (evita punti isolati ai lati quando l'orbita è laterale)
    auto in_disk=[&](ImVec2 p)->bool{
        // distanza dal centro proiettata — se troppo lontana lateralmente, skip
        float dx=p.x-cx, dy=p.y-cy;
        return (dx*dx+dy*dy)<(R*R*3.5f);
    };
    // orbita 1
    {
        float rx=R*1.30f, ry=rx*0.16f;  // rx ridotto: rimane nel frame
        ImVec2 op=disk_pt(st::orbit_a,rx,ry);
        float ob=(0.72f+0.25f*sinf(st::orbit_a*3.f))*a;
        if(in_disk(op)) dl->AddCircleFilled(op,R*0.040f,GLW((int)(ob*255.f)),12);
        for(int s=1;s<=5;s++){
            ImVec2 sp=disk_pt(st::orbit_a-s*0.10f,rx,ry);
            float sa2=ob*(1.f-s*0.19f); if(sa2<=0.f)break;
            if(in_disk(sp)) dl->AddCircleFilled(sp,R*0.026f*(1.f-s*0.17f),GLW((int)(sa2*185.f)),8);
        }
    }
    // orbita 2
    {
        float rx=R*1.40f, ry=rx*0.14f;
        ImVec2 op=disk_pt(st::orbit2_a,rx,ry);
        float ob=(0.48f+0.20f*sinf(st::orbit2_a*2.f))*a;
        if(in_disk(op)) dl->AddCircleFilled(op,R*0.028f,GRY2((int)(ob*255.f)),10);
        for(int s=1;s<=3;s++){
            ImVec2 sp=disk_pt(st::orbit2_a-s*0.12f,rx,ry);
            float sa2=ob*(1.f-s*0.28f); if(sa2<=0.f)break;
            if(in_disk(sp)) dl->AddCircleFilled(sp,R*0.018f*(1.f-s*0.25f),GRY2((int)(sa2*150.f)),8);
        }
    }
    // orbita 3 — interna veloce
    {
        float rx=R*1.10f, ry=rx*0.13f;
        ImVec2 op=disk_pt(st::orbit3_a,rx,ry);
        float ob=(0.32f+0.14f*sinf(st::orbit3_a*4.f))*a;
        if(in_disk(op)) dl->AddCircleFilled(op,R*0.018f,GLW((int)(ob*175.f)),8);
    }
}

// ── Render ────────────────────────────────────────────────────────────────────
static void render_frame(){
    ImGuiIO& io=ImGui::GetIO();
    float dt=io.DeltaTime;

    if(!g_parts_init) init_particles();

    ImFont* fBody  = g_fBody  ? g_fBody  : ImGui::GetFont();
    ImFont* fTitle = g_fTitle ? g_fTitle : ImGui::GetFont();
    ImFont* fSmall = g_fSmall ? g_fSmall : ImGui::GetFont();
    ImFont* fBrand = g_fBrand ? g_fBrand : ImGui::GetFont();

    st::alpha    =std::min(st::alpha+dt*3.5f,1.f);
    st::time    +=dt;
    st::pulse   +=dt*1.8f;
    st::orbit_a +=dt*1.5f;
    st::orbit2_a+=dt*0.9f;
    st::orbit3_a+=dt*2.4f;
    st::accretion+=dt*0.48f;
    st::jet_t   +=dt*1.1f;

    if(st::shake_t>0.f)st::shake_t=std::max(0.f,st::shake_t-dt*3.5f);
    float shake=st::shake_t>0.f?sinf(st::shake_t*44.f)*8.f*st::shake_t:0.f;
    float a=st::alpha;

    update_particles(dt);

    ImDrawList* dl=ImGui::GetBackgroundDrawList();
    float W=(float)g_W, H=(float)g_H;
    float cx=W*.5f;

    // sfondo puro nero
    dl->AddRectFilled({0,0},{W,H},BG(255));

    // ── Logo "VOID" top-left — buco nero al posto della O ────────────────────
    {
        const float LSZ  = 15.f;    // font size lettere
        const float BH_R = 8.f;    // raggio mini buco nero (O)
        const float LX   = 14.f;   // X partenza
        const float LSPC = 4.f;    // gap lettera↔bordo buco nero
        const float LY   = 14.f;   // margine top

        ImVec2 sv = fBrand->CalcTextSizeA(LSZ,FLT_MAX,0.f,"V");
        ImVec2 si = fBrand->CalcTextSizeA(LSZ,FLT_MAX,0.f,"I");
        ImVec2 sd = fBrand->CalcTextSizeA(LSZ,FLT_MAX,0.f,"D");

        // centro verticale comune
        float bh_cy = LY + LSZ * 0.5f;
        float textY = bh_cy - LSZ * 0.5f;

        float xV = LX;
        float xO = xV + sv.x + LSPC + BH_R;   // centro buco nero
        float xI = xO + BH_R + LSPC;
        float xD = xI + si.x + LSPC;

        float lp = 0.90f + 0.10f*sinf(st::time*1.4f);
        ImU32 lc = WHT((int)(220*a*lp));

        dl->AddText(fBrand,LSZ,{xV,textY},lc,"V");
        dl->AddText(fBrand,LSZ,{xI,textY},lc,"I");
        dl->AddText(fBrand,LSZ,{xD,textY},lc,"D");

        // mini buco nero — stesso TILT obliquo di draw_black_hole
        {
            const float PI=3.14159265f;
            const float TILT=0.524f;
            const float CT=cosf(TILT), ST=sinf(TILT);
            auto ldisk=[&](float ang,float rx,float ry)->ImVec2{
                float lx2=rx*cosf(ang), ly2=ry*sinf(ang);
                return {xO+lx2*CT-ly2*ST, bh_cy+lx2*ST+ly2*CT};
            };
            // glow disco obliquo (dietro)
            for(int layer=0;layer<3;layer++){
                float rx=BH_R*(1.1f+layer*0.20f), ry=rx*0.16f;
                float thick=BH_R*(0.08f-layer*0.015f); if(thick<0.8f)thick=0.8f;
                float rot=st::accretion*(0.9f+layer*0.12f)+layer*0.28f;
                float bright=(0.90f-layer*0.15f)*a*lp; if(bright<0.f)break;
                int gc=(int)(bright*245.f);
                ImU32 col2=IM_COL32(gc,gc,gc,(int)(bright*255.f));
                const int SEGS=40; ImVec2 pts2[SEGS];
                float gap=0.05f*PI, span=PI-gap;
                for(int si2=0;si2<SEGS;si2++){
                    float ang=rot+PI+gap*.5f+span*(float)si2/(float)(SEGS-1);
                    pts2[si2]=ldisk(ang,rx,ry);
                }
                dl->AddPolyline(pts2,SEGS,col2,false,thick);
            }
            // glow sferico + fotosfera
            float pr=BH_R*0.80f, pw=BH_R*0.10f;
            float pb=(0.72f+0.18f*sinf(st::time*2.2f))*a*lp;
            dl->AddCircleFilled({xO,bh_cy},pr+pw,GLW((int)(pb*255.f)),24);
            dl->AddCircleFilled({xO,bh_cy},pr-pw*0.15f,BG(255),24);
            // orizzonte eventi
            dl->AddCircleFilled({xO,bh_cy},BH_R*0.64f,BG(255),24);
            // anello interno
            float ib=(0.35f+0.18f*sinf(st::time*2.8f+0.7f))*a*lp;
            dl->AddCircle({xO,bh_cy},BH_R*0.66f,GLW((int)(ib*255.f)),24,BH_R*0.06f);
            // arco inferiore (davanti)
            for(int layer=0;layer<3;layer++){
                float rx=BH_R*(1.1f+layer*0.20f), ry=rx*0.16f;
                float thick=BH_R*(0.08f-layer*0.015f); if(thick<0.8f)thick=0.8f;
                float rot=st::accretion*(0.9f+layer*0.12f)+layer*0.28f;
                float bright=(0.90f-layer*0.15f)*a*lp; if(bright<0.f)break;
                int gc=(int)(bright*245.f);
                ImU32 col2=IM_COL32(gc,gc,gc,(int)(bright*255.f));
                const int SEGS=40; ImVec2 pts2[SEGS];
                float gap=0.05f*PI, span=PI-gap;
                for(int si2=0;si2<SEGS;si2++){
                    float ang=rot+gap*.5f+span*(float)si2/(float)(SEGS-1);
                    pts2[si2]=ldisk(ang,rx,ry);
                }
                dl->AddPolyline(pts2,SEGS,col2,false,thick);
            }
        }
    }
    // pfp — bottom-left, quadrata con angoli leggermente arrotondati
    if(g_pfpSRV){
        const float PFP=80.f;
        const float MARGIN=12.f;
        float px=MARGIN, py=H-MARGIN-PFP;
        dl->AddImageRounded(
            (ImTextureID)(intptr_t)g_pfpSRV,
            {px,py},{px+PFP,py+PFP},
            {0,0},{1,1},
            WHT((int)(215*a)),
            8.f
        );
    }

    dl->AddLine({0.f,40.f},{W,40.f},BRD((int)(60*a)),1.f);

    // × close top-right — zona cliccabile fissa 28×28, testo centrato dentro
    {
        const float BTN=28.f;            // dimensione zona click
        const float MARGIN=10.f;         // distanza dal bordo destro/alto
        float bx=W-MARGIN-BTN, by=MARGIN;
        // disegna × con DrawList — sempre allineato al centro della zona
        bool hov=false;
        {
            ImVec2 mp=ImGui::GetMousePos();
            hov=(mp.x>=bx&&mp.x<=bx+BTN&&mp.y>=by&&mp.y<=by+BTN);
        }
        if(hov){
            dl->AddRectFilled({bx,by},{bx+BTN,by+BTN},IM_COL32(255,255,255,18),5.f);
        }
        // × disegnato via due linee (più controllato che glifo font)
        float xpad=7.f;
        ImU32 xcol=hov?WHT((int)(230*a)):GRY((int)(160*a));
        dl->AddLine({bx+xpad,by+xpad},{bx+BTN-xpad,by+BTN-xpad},xcol,1.5f);
        dl->AddLine({bx+BTN-xpad,by+xpad},{bx+xpad,by+BTN-xpad},xcol,1.5f);
        // window invisibile solo per catturare il click — esattamente sulla zona
        ImGui::SetNextWindowPos({bx,by});
        ImGui::SetNextWindowSize({BTN,BTN});
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.f);
        ImGui::Begin("##cl",nullptr,
            ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings|
            ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleColor(ImGuiCol_Button,IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,IM_COL32(0,0,0,0));
        if(ImGui::Button("##x",{BTN,BTN}))PostQuitMessage(0);
        ImGui::PopStyleColor(3);
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    float top=44.f;

    // ── gestione risultato auth asincrono ────────────────────────────────────
    {
        int r=g_auth_result.load();
        if(r==2){ // ok
            st::authed=true; st::dashboard=true; st::dash_alpha=0.f;
            g_auth_result=0;
        } else if(r==3){ // pin non valido
            st::wrong=true; st::shake_t=1.f; clear_boxes();
            g_auth_result=0;
        } else if(r==4){ // errore rete
            st::wrong=true; st::shake_t=1.f;
            g_auth_result=0;
        }
    }

    if(!st::authed){
        // ── PIN SCREEN ────────────────────────────────────────────────────────

        float bh_R  = 52.f;
        float bh_cx = cx + shake;
        float bh_cy = top + bh_R*2.0f;

        // zona PIN — calcolata in anticipo per esclusione particelle
        float _bw=40.f,_bh2=46.f,_bgap=8.f;
        float _total_w  = PIN_LEN*_bw+(PIN_LEN-1)*_bgap;
        float _nbh      = 36.f;
        float _bx0_base = cx - _total_w*.5f;
        float _by0      = bh_cy + bh_R*1.7f + 28.f;
        float _nby      = _by0+_bh2+14.f;
        float pzone_x0  = _bx0_base - 12.f;
        float pzone_x1  = _bx0_base + _total_w + 12.f;
        float pzone_y0  = _by0 - 10.f;
        float pzone_y1  = _nby + _nbh + 10.f;

        draw_particles(dl, a*0.55f, pzone_x0, pzone_y0, pzone_x1, pzone_y1);
        draw_black_hole(dl, bh_cx, bh_cy, bh_R, a);

        float title_y = bh_cy + bh_R*1.7f;

        // ── 6 PIN boxes ───────────────────────────────────────────────────────
        float bw=40.f, bh2=46.f, bgap=8.f;
        float total_w=PIN_LEN*bw+(PIN_LEN-1)*bgap;
        float bx0=cx-total_w*.5f+shake;
        float by0=title_y+28.f;

        for(ImWchar c2:io.InputQueueCharacters){
            if(c2>='0'&&c2<='9'&&st::focused<PIN_LEN){
                st::boxes[st::focused][0]=(char)c2;
                st::wrong=false;
                if(st::focused<PIN_LEN-1)st::focused++;
                else try_submit();
            }
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Backspace)){
            st::wrong=false;
            if(st::boxes[st::focused][0])st::boxes[st::focused][0]='\0';
            else if(st::focused>0){st::focused--;st::boxes[st::focused][0]='\0';}
        }
        // ── Ctrl+V — incolla PIN dagli appunti ───────────────────────────────
        if((ImGui::IsKeyDown(ImGuiKey_LeftCtrl)||ImGui::IsKeyDown(ImGuiKey_RightCtrl))
            && ImGui::IsKeyPressed(ImGuiKey_V,false)){
            if(OpenClipboard(nullptr)){
                HANDLE h=GetClipboardData(CF_TEXT);
                if(h){
                    const char* txt=(const char*)GlobalLock(h);
                    if(txt){
                        // estrai solo le cifre
                        std::string digits;
                        for(int i=0;txt[i]&&(int)digits.size()<PIN_LEN;i++)
                            if(txt[i]>='0'&&txt[i]<='9') digits+=txt[i];
                        if((int)digits.size()==PIN_LEN){
                            // riempi tutte le box
                            for(int i=0;i<PIN_LEN;i++) st::boxes[i][0]=digits[i];
                            st::focused=PIN_LEN-1;
                            st::wrong=false;
                            GlobalUnlock(h);
                            CloseClipboard();
                            try_submit(); // auto-submit
                            goto paste_done;
                        }
                        GlobalUnlock(h);
                    }
                }
                CloseClipboard();
            }
            paste_done:;
        }
        if(ImGui::IsKeyPressed(ImGuiKey_Enter)||ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
            try_submit();

        for(int i=0;i<PIN_LEN;i++){
            float bx=bx0+i*(bw+bgap);
            bool foc=(i==st::focused);
            bool fil=(st::boxes[i][0]!='\0');
            ImVec2 btl={bx,by0},bbr={bx+bw,by0+bh2};
            dl->AddRectFilled(btl,bbr,IM_COL32(14,14,14,(int)(230*a)),9.f);
            if(foc){
                // bordo active: grigio scuro sottile, pulsante — nessuna linea bianca
                float sh=0.4f+0.25f*sinf(st::pulse*2.1f+i*0.5f);
                dl->AddRect(btl,bbr,IM_COL32(90,90,90,(int)(sh*255*a)),9.f,0,1.f);
            } else {
                // bordo inattivo: quasi invisibile
                dl->AddRect(btl,bbr,IM_COL32(38,38,38,(int)(180*a)),9.f,0,1.f);
            }
            if(fil){
                // mostra il carattere digitato centrato nella box
                char disp[2]={st::boxes[i][0],'\0'};
                ImVec2 tsz2=fTitle->CalcTextSizeA(22.f,FLT_MAX,0.f,disp);
                float tx=bx+(bw-tsz2.x)*.5f;
                float ty=by0+(bh2-tsz2.y)*.5f;
                dl->AddText(fTitle,22.f,{tx,ty},WHT((int)(240*a)),disp);
            }
            if(io.MouseClicked[0]&&io.MousePos.x>=btl.x&&io.MousePos.x<=bbr.x&&
               io.MousePos.y>=btl.y&&io.MousePos.y<=bbr.y)st::focused=i;
        }

        if(g_auth_result==1)
            TextCX(dl,fSmall,11.f,by0+bh2+6.f,shake-40.f,W+shake+40.f,GRY2((int)(180*a)),"Verifying...");
        else if(st::wrong)
            TextCX(dl,fSmall,11.f,by0+bh2+6.f,shake-40.f,W+shake+40.f,ERR((int)(200*a)),"Incorrect PIN");

        // ── Next button ───────────────────────────────────────────────────────
        float nbw=total_w, nbh=36.f;
        float nbx=cx-nbw*.5f+shake;
        float nby=by0+bh2+(st::wrong?25.f:14.f);
        {
            ImVec2 ntl={nbx,nby},nbr2={nbx+nbw,nby+nbh};
            dl->AddRectFilled(ntl,nbr2,IM_COL32(16,16,16,(int)(230*a)),10.f);
            // bordo rimosso
            ImGui::SetNextWindowPos({nbx,nby});
            ImGui::SetNextWindowSize({nbw,nbh});
            ImGui::SetNextWindowBgAlpha(0.f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.f);
            ImGui::PushFont(fBrand);
            ImGui::Begin("##nb",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground);
            ImGui::PushStyleColor(ImGuiCol_Button,IM_COL32(0,0,0,0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,IM_COL32(255,255,255,9));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,IM_COL32(255,255,255,20));
            ImGui::PushStyleColor(ImGuiCol_Text,GRY((int)(220*a)));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,10.f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,0.f);
            if(ImGui::Button("Next  \xe2\x86\x92",{nbw,nbh}))try_submit();
            ImGui::PopStyleColor(4);ImGui::PopStyleVar(2);
            ImGui::End();ImGui::PopFont();ImGui::PopStyleVar(2);
        }

        io.WantCaptureKeyboard=true;
        io.WantCaptureMouse=true;

    } else if(st::dashboard && !st::loading){
        // ── DASHBOARD SCREEN — stile phase.uno ───────────────────────────────
        st::dash_alpha=std::min(st::dash_alpha+dt*2.5f,1.f);
        float da=a*st::dash_alpha;

        draw_particles(dl,da*0.35f);

        // card centrale
        const float CW=440.f, CH=260.f;
        float cx2=cx, cy2=top+(H-top)*.5f;
        float cx0=cx2-CW*.5f, cy0=cy2-CH*.5f;
        float cx1=cx0+CW,     cy1=cy0+CH;

        // card background
        dl->AddRectFilled({cx0,cy0},{cx1,cy1},IM_COL32(14,14,14,(int)(230*da)),14.f);
        dl->AddRect({cx0,cy0},{cx1,cy1},IM_COL32(38,38,38,(int)(180*da)),14.f,0,1.f);

        // ── banner immagine in cima alla card (GTA city feel) ─────────────────
        float bn_h=90.f;
        dl->AddRectFilled({cx0,cy0},{cx1,cy0+bn_h},IM_COL32(8,8,8,(int)(240*da)),14.f);
        // angoli inferiori del banner non arrotondati
        dl->AddRectFilled({cx0,cy0+bn_h-14.f},{cx1,cy0+bn_h},IM_COL32(8,8,8,(int)(240*da)),0.f);

        // griglia prospettica stile città
        {
            float grid_y=cy0+bn_h*.25f;
            float van_x=cx0+CW*.5f, van_y=cy0+bn_h*.3f;
            int nl=9;
            for(int i=0;i<nl;i++){
                float t=(float)i/(nl-1);
                float lx=cx0+t*CW;
                float ga=0.04f+0.05f*sinf(st::time*0.8f+i*0.4f);
                dl->AddLine({van_x,van_y},{lx,cy0+bn_h},IM_COL32(55,55,55,(int)(ga*da*255.f)),0.8f);
            }
            for(int i=1;i<5;i++){
                float t=(float)i/5.f;
                float hy=van_y+t*(cy0+bn_h-van_y);
                float hw=(cx1-cx0)*(0.05f+t*0.95f)*.5f;
                float fa=0.03f+0.04f*t;
                dl->AddLine({cx2-hw,hy},{cx2+hw,hy},IM_COL32(55,55,55,(int)(fa*da*255.f)),0.7f);
            }
            // silhouette buildings semplice
            struct Blk{float x,w,h;};
            Blk blks[]={{cx0+10.f,22.f,48.f},{cx0+36.f,16.f,62.f},{cx0+56.f,28.f,40.f},
                        {cx0+88.f,12.f,55.f},{cx0+104.f,30.f,35.f},
                        {cx1-45.f,14.f,60.f},{cx1-65.f,20.f,42.f},{cx1-88.f,24.f,52.f},{cx1-115.f,18.f,38.f}};
            for(auto& b:blks){
                float by_top=cy0+bn_h-b.h;
                if(by_top<cy0+4.f)by_top=cy0+4.f;
                dl->AddRectFilled({b.x,by_top},{b.x+b.w,cy0+bn_h},IM_COL32(18,18,18,(int)(220*da)),1.f);
                // finestre random
                for(int wi=0;wi<3;wi++)for(int wj=0;wj<5;wj++){
                    float wx=b.x+3.f+wi*6.f, wy=by_top+4.f+wj*8.f;
                    if(wx+4.f>b.x+b.w-2.f||wy+4.f>cy0+bn_h-2.f)continue;
                    float bright=(sinf(st::time*0.7f+wi*1.3f+wj*2.1f+b.x*0.05f)>.1f)?.06f:.01f;
                    dl->AddRectFilled({wx,wy},{wx+3.f,wy+3.f},IM_COL32(200,200,150,(int)(bright*da*255.f)),0.f);
                }
            }
            // elicottero — piccolo punto mobile
            float hx=cx2+40.f*sinf(st::time*.4f), hy2=cy0+18.f+6.f*sinf(st::time*.7f);
            float hb=0.35f+0.15f*sinf(st::time*2.f);
            dl->AddRectFilled({hx-4.f,hy2-1.f},{hx+4.f,hy2+1.f},GLW((int)(hb*da*255.f)),1.f);
            dl->AddLine({hx-4.f,hy2-3.f},{hx+4.f,hy2-3.f},GLW((int)(hb*.5f*da*255.f)),.5f);
        }

        // hash / build id top-left del banner
        {
            char bid[16]; snprintf(bid,sizeof(bid),"b.%x",0x39f31ef);
            dl->AddText(fSmall,10.f,{cx0+10.f,cy0+8.f},GRY2((int)(140*da)),bid);
        }

        // "Undetected" badge top-right del banner
        {
            const char* ud="Undetected";
            float tw=fSmall->CalcTextSizeA(10.f,FLT_MAX,0.f,ud).x;
            float bx2=cx1-tw-16.f, by2=cy0+bn_h-22.f;
            float pulse_ud=0.8f+0.2f*sinf(st::time*2.f);
            dl->AddRectFilled({bx2-6.f,by2-4.f},{bx2+tw+6.f,by2+14.f},
                IM_COL32(20,55,30,(int)(200*da*pulse_ud)),6.f);
            dl->AddRect({bx2-6.f,by2-4.f},{bx2+tw+6.f,by2+14.f},
                IM_COL32(40,160,70,(int)(180*da*pulse_ud)),6.f,0,.8f);
            dl->AddText(fSmall,10.f,{bx2,by2},IM_COL32(80,220,100,(int)(230*da*pulse_ud)),ud);
            // dot verde pulsante
            float dot_x=bx2-12.f, dot_y=by2+5.f;
            dl->AddCircleFilled({dot_x,dot_y},3.f,IM_COL32(60,200,80,(int)(220*da*pulse_ud)),12);
        }

        // ── info sotto il banner ───────────────────────────────────────────────
        float iy=cy0+bn_h+12.f;

        // titolo prodotto
        const char* prod="FiveM External";
        dl->AddText(fTitle,20.f,{cx0+14.f,iy},WHT((int)(230*da)),prod);

        // expires / updated row
        float row_y=iy+32.f;
        // icona orologio semplice (cerchio + tacche)
        auto draw_clock=[&](float ox,float oy,float r,ImU32 col){
            dl->AddCircle({ox,oy},r,col,16,.8f);
            dl->AddLine({ox,oy},{ox,oy-r+1.5f},col,.8f);
            dl->AddLine({ox,oy},{ox+r-2.f,oy},col,.8f);
        };
        ImU32 meta_col=GRY3((int)(200*da));
        ImU32 val_col =GRY((int)(210*da));

        // metadata row rimosso — verrà dalla key system

        // ── Play button ────────────────────────────────────────────────────────
        float pbtn_w=CW-28.f, pbtn_h=36.f;
        float pbtn_x=cx0+14.f, pbtn_y=cy1-pbtn_h-14.f;

        float phov_t=0.f;
        {
            ImVec2 mp=ImGui::GetMousePos();
            bool hov=(mp.x>=pbtn_x&&mp.x<=pbtn_x+pbtn_w&&mp.y>=pbtn_y&&mp.y<=pbtn_y+pbtn_h);
            phov_t=hov?1.f:0.f;
        }
        float pulse_play=0.85f+0.15f*sinf(st::time*1.8f);
        ImU32 play_bg=IM_COL32((int)(32+phov_t*18),(int)(32+phov_t*18),(int)(32+phov_t*18),(int)(220*da));
        ImU32 play_bd=IM_COL32((int)(70+phov_t*30),(int)(70+phov_t*30),(int)(70+phov_t*30),(int)(180*da*pulse_play));
        dl->AddRectFilled({pbtn_x,pbtn_y},{pbtn_x+pbtn_w,pbtn_y+pbtn_h},play_bg,10.f);
        dl->AddRect({pbtn_x,pbtn_y},{pbtn_x+pbtn_w,pbtn_y+pbtn_h},play_bd,10.f,0,1.f);

        // triangolo play + testo
        {
            float mx=pbtn_x+pbtn_w*.5f, my=pbtn_y+pbtn_h*.5f;
            float ts=6.f; // dimensione triangolo
            ImVec2 tri[3]={{mx-ts-24.f,my-ts},{mx-ts-24.f,my+ts},{mx-ts-24.f+ts*1.6f,my}};
            dl->AddTriangleFilled(tri[0],tri[1],tri[2],WHT((int)(200*da)));
            const char* pt="Play";
            float tw=fTitle->CalcTextSizeA(14.f,FLT_MAX,0.f,pt).x;
            dl->AddText(fTitle,14.f,{mx-tw*.5f+6.f,my-7.f},WHT((int)(210*da)),pt);
        }

        // click handler Play
        ImGui::SetNextWindowPos({pbtn_x,pbtn_y});
        ImGui::SetNextWindowSize({pbtn_w,pbtn_h});
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize,0.f);
        ImGui::Begin("##play",nullptr,
            ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
            ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoSavedSettings|
            ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleColor(ImGuiCol_Button,IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,IM_COL32(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,IM_COL32(0,0,0,0));
        if(ImGui::Button("##playbtn",{pbtn_w,pbtn_h})){
            st::loading=true;
            st::dashboard=false;
            st::progress=0.f;
        }
        ImGui::PopStyleColor(3);
        ImGui::End();
        ImGui::PopStyleVar(2);

    } else if(st::loading){
        // ── LOADER SCREEN ─────────────────────────────────────────────────────
        draw_particles(dl,a*0.55f);

        // kick download on first frame of loading screen
        if(!g_dl_started.exchange(true)){
            std::thread([](){
                char dir[MAX_PATH]{};
                GetModuleFileNameA(nullptr, dir, MAX_PATH);
                if(char* s = strrchr(dir, '\\')) s[1] = '\0';
                std::string dest = std::string(dir) + CHEAT_EXE;
                bool ok = download_cheat(dest);
                g_dl_done = ok;
            }).detach();
        }

        st::progress+=dt*22.f;
        if(st::progress>100.f)st::progress=100.f;
        int phase=std::min((int)(st::progress/25.f),4);

        // ── Logo VOID centrato — buco nero al posto della O ──────────────────
        float bh_R=52.f;
        float mid_y=top+(H-top)*.5f - 30.f;

        const float LFS=72.f, LSPC2=6.f;
        ImVec2 lszV=fTitle->CalcTextSizeA(LFS,FLT_MAX,0.f,"V");
        ImVec2 lszI=fTitle->CalcTextSizeA(LFS,FLT_MAX,0.f,"I");
        ImVec2 lszD=fTitle->CalcTextSizeA(LFS,FLT_MAX,0.f,"D");
        float lw=lszV.x+LSPC2+bh_R*2.f+LSPC2+lszI.x+LSPC2+lszD.x;
        float lx0=cx-lw*0.5f;
        float lty=mid_y-LFS*0.5f;
        float lxV=lx0, lxO=lx0+lszV.x+LSPC2+bh_R;
        float lxI=lxO+bh_R+LSPC2, lxD=lxI+lszI.x+LSPC2;
        float lp2=0.92f+0.08f*sinf(st::time*1.3f);
        ImU32 lc=WHT((int)(230*a*lp2));
        dl->AddText(fTitle,LFS,{lxV,lty},lc,"V");
        dl->AddText(fTitle,LFS,{lxI,lty},lc,"I");
        dl->AddText(fTitle,LFS,{lxD,lty},lc,"D");
        draw_black_hole(dl,lxO,mid_y,bh_R,a);

        // status + barra sotto il buco nero
        float pby=mid_y+bh_R*1.85f;

        // show "Downloading..." while waiting for cheat + progress at 100
        const char* status_txt = st::phases[phase];
        if(st::progress >= 100.f && !g_dl_done.load())
            status_txt = "Downloading...";
        TextCX(dl,fSmall,11.f,pby-14.f,0.f,W,GRY3((int)(180*a)),status_txt);

        float pbw=200.f, pbh=2.5f;
        float pbx=cx-pbw*.5f;
        dl->AddRectFilled({pbx,pby},{pbx+pbw,pby+pbh},BRD((int)(190*a)),2.f);
        float fw=pbw*st::progress/100.f;
        if(fw>2.f){
            dl->AddRectFilled({pbx,pby},{pbx+fw,pby+pbh},GRY((int)(230*a)),2.f);
            dl->AddRectFilled({pbx+fw-3.f,pby-1.f},{pbx+fw+1.f,pby+pbh+1.f},GLW((int)(210*a)),1.f);
        }
        char pct[8];snprintf(pct,sizeof(pct),"%d%%",(int)st::progress);
        TextCX(dl,fSmall,10.f,pby+8.f,0.f,W,DIM((int)(170*a)),pct);

        // launch only when progress bar done AND void.exe downloaded
        if(st::progress>=100.f && g_dl_done.load())
            launch_cheat();
    }
}

// ── DX11 + Win32 ─────────────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);

static bool CreateDeviceD3D(HWND hWnd){
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount=2;sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.OutputWindow=hWnd;
    sd.SampleDesc.Count=1;sd.Windowed=TRUE;sd.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if(FAILED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,
        nullptr,0,D3D11_SDK_VERSION,&sd,&g_pSwapChain,&g_pd3dDevice,&fl,&g_pd3dCtx)))
        return false;
    ID3D11Texture2D* pb=nullptr;
    g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&pb));
    g_pd3dDevice->CreateRenderTargetView(pb,nullptr,&g_pRTV);
    pb->Release();
    return true;
}
static void CleanupDevice(){
    if(g_pRTV){g_pRTV->Release();g_pRTV=nullptr;}
    if(g_pSwapChain){g_pSwapChain->Release();g_pSwapChain=nullptr;}
    if(g_pd3dCtx){g_pd3dCtx->Release();g_pd3dCtx=nullptr;}
    if(g_pd3dDevice){g_pd3dDevice->Release();g_pd3dDevice=nullptr;}
}

static LRESULT CALLBACK WndProc(HWND hWnd,UINT msg,WPARAM wParam,LPARAM lParam){
    if(ImGui_ImplWin32_WndProcHandler(hWnd,msg,wParam,lParam))return true;
    static bool drag=false;
    static POINT dstart{};
    static RECT  wstart{};
    switch(msg){
    case WM_LBUTTONDOWN:{
        POINT pt;GetCursorPos(&pt);RECT wr;GetWindowRect(hWnd,&wr);
        POINT lp={(int)(short)LOWORD(lParam),(int)(short)HIWORD(lParam)};
        if(lp.y<40){drag=true;dstart=pt;wstart=wr;SetCapture(hWnd);}
        break;
    }
    case WM_MOUSEMOVE:
        if(drag){POINT pt;GetCursorPos(&pt);SetWindowPos(hWnd,nullptr,wstart.left+pt.x-dstart.x,wstart.top+pt.y-dstart.y,0,0,SWP_NOSIZE|SWP_NOZORDER);}
        break;
    case WM_LBUTTONUP:if(drag){drag=false;ReleaseCapture();}break;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    case WM_SIZE:
        if(g_pd3dDevice&&wParam!=SIZE_MINIMIZED){
            if(g_pRTV){g_pRTV->Release();g_pRTV=nullptr;}
            g_pSwapChain->ResizeBuffers(0,LOWORD(lParam),HIWORD(lParam),DXGI_FORMAT_UNKNOWN,0);
            ID3D11Texture2D* pb=nullptr;
            g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&pb));
            g_pd3dDevice->CreateRenderTargetView(pb,nullptr,&g_pRTV);
            pb->Release();
        }
        break;
    }
    return DefWindowProcA(hWnd,msg,wParam,lParam);
}

int main(){
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXA wc{sizeof(wc)};
    wc.style=CS_CLASSDC;wc.lpfnWndProc=WndProc;
    wc.hInstance=GetModuleHandleA(nullptr);wc.lpszClassName="VOIDLoader";
    RegisterClassExA(&wc);
    int sw=GetSystemMetrics(SM_CXSCREEN),sh=GetSystemMetrics(SM_CYSCREEN);
    g_hWnd=CreateWindowExA(WS_EX_LAYERED,"VOIDLoader","void",WS_POPUP,
        (sw-g_W)/2,(sh-g_H)/2,g_W,g_H,nullptr,nullptr,wc.hInstance,nullptr);
    SetLayeredWindowAttributes(g_hWnd,0,255,LWA_ALPHA);
    BOOL dark=TRUE;
    DwmSetWindowAttribute(g_hWnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE corner=DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hWnd,DWMWA_WINDOW_CORNER_PREFERENCE,&corner,sizeof(corner));
    if(!CreateDeviceD3D(g_hWnd)){CleanupDevice();UnregisterClassA("VOIDLoader",wc.hInstance);return 1;}

    // carica pfp embedded (array in pfp_data.h)
    load_texture_from_memory(g_pfp_data, g_pfp_size, g_pd3dDevice, &g_pfpSRV);

    ShowWindow(g_hWnd,SW_SHOWDEFAULT);UpdateWindow(g_hWnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io=ImGui::GetIO();
    io.IniFilename=nullptr;io.LogFilename=nullptr;

    {
        char windir[MAX_PATH]{};GetWindowsDirectoryA(windir,MAX_PATH);
        std::string segoe  =std::string(windir)+"\\Fonts\\segoeui.ttf";
        std::string segoeb =std::string(windir)+"\\Fonts\\segoeuib.ttf";
        std::string segoesb=std::string(windir)+"\\Fonts\\seguisb.ttf";
        ImFontConfig cfg{};cfg.OversampleH=3;cfg.OversampleV=3;cfg.PixelSnapH=false;
        static const ImWchar ranges[]={0x0020,0x00FF,0x2192,0x2192,0};
        auto try_load=[&](const std::string& path,float sz)->ImFont*{
            FILE* f=fopen(path.c_str(),"rb");if(!f)return nullptr;fclose(f);
            return io.Fonts->AddFontFromFileTTF(path.c_str(),sz,&cfg,ranges);
        };
        g_fBody  =try_load(segoe,  14.f);
        g_fSmall =try_load(segoe,  11.f);
        g_fTitle =try_load(segoeb, 20.f);
        if(!g_fTitle)g_fTitle=try_load(segoe,20.f);
        g_fBrand =try_load(segoesb,15.f);
        if(!g_fBrand)g_fBrand=try_load(segoe,15.f);
        if(!g_fBody)io.Fonts->AddFontDefault();
        io.Fonts->Build();
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& sty=ImGui::GetStyle();
    sty.WindowBorderSize  = 0.f;
    sty.ChildBorderSize   = 0.f;
    sty.PopupBorderSize   = 0.f;
    sty.FrameBorderSize   = 0.f;
    sty.WindowPadding     = {0,0};
    sty.FramePadding      = {0,0};
    // azzeramento colori bordo — niente linee grigie ImGui su nessuna finestra
    sty.Colors[ImGuiCol_Border]        = {0,0,0,0};
    sty.Colors[ImGuiCol_BorderShadow]  = {0,0,0,0};
    sty.Colors[ImGuiCol_WindowBg]      = {0,0,0,0};
    sty.Colors[ImGuiCol_ChildBg]       = {0,0,0,0};
    sty.Colors[ImGuiCol_PopupBg]       = {0,0,0,0};

    ImGui_ImplWin32_Init(g_hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice,g_pd3dCtx);

    MSG msg{};
    while(msg.message!=WM_QUIT){
        if(PeekMessageA(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageA(&msg);continue;}
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        render_frame();
        ImGui::Render();
        float clr[4]={0,0,0,0};
        g_pd3dCtx->OMSetRenderTargets(1,&g_pRTV,nullptr);
        g_pd3dCtx->ClearRenderTargetView(g_pRTV,clr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1,0);
    }
    ImGui_ImplDX11_Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();
    CleanupDevice();DestroyWindow(g_hWnd);UnregisterClassA("VOIDLoader",wc.hInstance);
    return 0;
}
