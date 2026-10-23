#include "game_menu.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
static void check(bool b, const char* why) { if(!b){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);} }
static bool crowded=false;
static void frame(bool expect) {
  ImGui::NewFrame(); if(crowded) {auto* dl=ImGui::GetForegroundDrawList();for(int i=0;i<16000;++i)dl->AddRectFilled(ImVec2(0,0),ImVec2(1,1),0xffffffff);}
  tc3::menu::Render(); ImGui::Render();
  ImDrawData* data=ImGui::GetDrawData();
  check((data->TotalVtxCount>0)==expect,"visibility geometry");
  for(const ImDrawList* list:data->CmdLists) for(const ImDrawCmd& cmd:list->CmdBuffer) {
    check(cmd.IdxOffset+cmd.ElemCount<=unsigned(list->IdxBuffer.Size),"index range");
    for(unsigned i=0;i<cmd.ElemCount;i++) check(cmd.VtxOffset+list->IdxBuffer[cmd.IdxOffset+i]<unsigned(list->VtxBuffer.Size),"vertex index");
  }
}
static void key(SDL_Keycode k){SDL_Event e{};e.type=SDL_EVENT_KEY_DOWN;e.key.key=k;check(tc3::menu::HandleEvent(e),"key consumed");}
static void click(float x,float y) {
  SDL_Event e{}; e.type=SDL_EVENT_MOUSE_BUTTON_DOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;
  check(tc3::menu::HandleEvent(e),"mouse down consumed");frame(true);
  e.type=SDL_EVENT_MOUSE_BUTTON_UP;check(tc3::menu::HandleEvent(e),"mouse up consumed");frame(true);
}
int main(){
  ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(1280,720);io.DeltaTime=1.0f/60;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset; io.Fonts->AddFontDefault();
  std::vector<tc3::menu::Action> actions;tc3::menu::SetActionCallback([&](auto a){actions.push_back(a);});
  using tc3::rendering::Option;
  std::vector<std::pair<Option,int>> changes;
  tc3::menu::SetOptionCallback([&](auto option,int direction){changes.emplace_back(option,direction);});
  tc3::menu::Snapshot snapshot{29.97,59.94,true,false,1280,720};
  snapshot.rendering.ready=snapshot.rendering.cas_supported=true;
  tc3::menu::PublishSnapshot(snapshot);
  frame(false); key(SDLK_F10);check(tc3::menu::IsOpen(),"open");check(actions.back()==tc3::menu::Action::Pause,"pause action");frame(true);
  key(SDLK_RIGHT); frame(true); key(SDLK_DOWN); frame(true); key(SDLK_RETURN); frame(true);
  check(actions.back()==tc3::menu::Action::ToggleFullscreen,"fullscreen action");
  key(SDLK_TAB);frame(true);key(SDLK_DOWN);key(SDLK_RETURN);frame(true);
  check(changes.back()==std::pair{Option::VSync,1},"VSync is connected");
  key(SDLK_DOWN);key(SDLK_LEFT);frame(true);
  check(changes.back()==std::pair{Option::QueueDepth,-1},"left reduces presentation queue");
  key(SDLK_TAB);key(SDLK_DOWN);key(SDLK_RIGHT);frame(true);
  check(changes.back()==std::pair{Option::Resolution,1},"right increases internal resolution");
  key(SDLK_LEFT);frame(true);
  check(changes.back()==std::pair{Option::Resolution,-1},"left reduces internal resolution");
  key(SDLK_DOWN);key(SDLK_RETURN);frame(true);
  check(changes.back()==std::pair{Option::FXAA,1},"FXAA is connected");
  key(SDLK_DOWN);key(SDLK_RIGHT);frame(true);
  check(changes.back()==std::pair{Option::CAS,1},"CAS is connected");
  auto count=changes.size();key(SDLK_DOWN);key(SDLK_RIGHT);frame(true);
  check(changes.size()==count,"sharpness disabled until CAS applied");
  snapshot.rendering.cas=1;tc3::menu::PublishSnapshot(snapshot);
  key(SDLK_RIGHT);frame(true);
  check(changes.back()==std::pair{Option::Sharpness,1},"sharpness enabled with actual CAS state");
  snapshot.rendering.cas_supported=false;tc3::menu::PublishSnapshot(snapshot);
  count=changes.size();key(SDLK_UP);key(SDLK_LEFT);frame(true);
  check(changes.size()==count,"unsupported CAS never dispatches");
  key(SDLK_END);key(SDLK_RIGHT);frame(true);
  check(changes.size()==count,"unconnected temporal features never dispatch");
  key(SDLK_ESCAPE);check(actions.back()==tc3::menu::Action::Resume,"resume action");frame(false);
  // Rendering -> Cabinet. Maintenance must dispatch the original board action
  // and resume execution so the guest can process its test switch.
  key(SDLK_F10);key(SDLK_TAB);key(SDLK_HOME);key(SDLK_RETURN);frame(true);
  check(!tc3::menu::IsOpen(),"maintenance resumes guest");
  check(actions.size()>1 && actions[actions.size()-2]==tc3::menu::Action::CabinetTest,"cabinet test action");
  frame(false);
  // Cabinet -> File -> Display. TC4 cannot accidentally launch the TC3 pair.
  snapshot.twin_supported=false;tc3::menu::PublishSnapshot(snapshot);
  key(SDLK_F10);key(SDLK_TAB);key(SDLK_TAB);key(SDLK_HOME);key(SDLK_DOWN);frame(true);
  auto action_count=actions.size();key(SDLK_RETURN);frame(true);
  check(actions.size()==action_count,"unsupported game twin launch blocked");
  key(SDLK_DOWN);key(SDLK_RIGHT);frame(true);
  check(changes.back()==std::pair{Option::SindenBorder,1},"Sinden detection/border option");
  key(SDLK_DOWN);key(SDLK_LEFT);frame(true);
  check(changes.back()==std::pair{Option::BorderWidth,-1},"per-view border size option");
  key(SDLK_ESCAPE);frame(false);
  key(SDLK_F10);frame(true); key(SDLK_TAB); frame(true); key(SDLK_TAB); frame(true);
  for(int i=0;i<5;i++){SDL_Event e{};e.type=SDL_EVENT_MOUSE_WHEEL;e.wheel.y=-3;tc3::menu::HandleEvent(e);frame(true);}
  crowded=true;frame(true);crowded=false;
  key(SDLK_ESCAPE);frame(false);tc3::menu::Shutdown();ImGui::DestroyContext();
  std::puts("PASS menu navigation, real rendering actions, capability gates, draw index ranges and 64k vertex-offset rollover");
}
