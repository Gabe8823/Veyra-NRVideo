// Run the selector notification path from a fresh all-effects-off session.
bool smokeFgOnly=false;int fgOnlyStep=0;
void tickFgOnlyChecks(HWND hwnd,const veyra::engine::PlayerSnapshot& s){
    if(!smokeFgOnly||fgOnlyStep<0||fgOnlyStep==4||!s.frames||s.applying||masterPendingRevision||transition.running)return;
    auto select=[&](int id,int index){auto control=veyra::ui::settingsControlForTest(id);SendMessageW(control,CB_SETCURSEL,index,0);SendMessageW(inspector,WM_COMMAND,MAKEWPARAM(id,CBN_SELCHANGE),LPARAM(control));};
    auto check=[&](bool ok,const char* name){veyra::log::info("ui-fg-only-test",std::format("{}={}",name,ok));if(!ok)fgOnlyStep=-1;return ok;};
    switch(fgOnlyStep){
    case 0:
        if(uiState.mode==veyra::ui::Mode::Daily){switchMode();return;}
        if(!check(!uiState.enhanced&&!s.applied.nr&&!s.applied.sr&&s.applied.multiplier==1,"fresh_session_all_effects_off"))break;
        selectInspector(1);select(202,1);fgOnlyStep=1;break;
    case 1:
        if(!uiState.enhanced||s.applied.multiplier!=2||s.generated<5)return;
        if(!check(!s.applied.nr&&!s.applied.sr&&s.nrEvaluated==0&&s.nvofExecuted>0&&s.fgActive,"DLSS_selector_enables_master_flow_and_real_FG_without_NR"))break;
        SendMessageW(hwnd,WM_COMMAND,Master,0);fgOnlyStep=2;break;
    case 2:
        if(uiState.enhanced||s.applied.multiplier!=1)return;
        select(208,int(veyra::engine::FrameGenerationBackend::XeSS));select(202,1);fgOnlyStep=3;break;
    case 3:
        if(!uiState.enhanced||s.applied.frameGenerationBackend!=veyra::engine::FrameGenerationBackend::XeSS||s.generated<5)return;
        if(!check(!s.applied.nr&&!s.applied.sr&&s.nrEvaluated==0&&s.nvofExecuted>0&&s.fgActive,"XeSS_selector_enables_master_flow_and_real_FG_without_NR"))break;
        fgOnlyStep=4;veyra::log::info("ui-fg-only-test","PASS cold UI selectors, actual optical flow, DLSS and XeSS generation, NR never evaluated");break;
    }
}
