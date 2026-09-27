globalThis.__drc={state:'running',ph:'open'};
(async()=>{
 const g=globalThis.__drc,out={};
 try{ await eda.dmt_EditorControl.openDocument('cf8920f8e3284a24'); }catch(e){out.eOpen=''+e;}
 try{ await eda.pcb_Document.stopCalculatingRatline(); }catch(e){}
 g.ph='ratline';
 try{ out.startRat=await eda.pcb_Document.startCalculatingRatline(); }catch(e){out.eRat=''+e;}
 for(var i=0;i<60;i++){ let s; try{ s=await eda.pcb_Document.getCalculatingRatlineStatus(); }catch(e){ s='err'; }
   if(!s) break; await new Promise(r=>setTimeout(r,500)); }
 g.ph='drc';
 try{ out.drc=await eda.pcb_Drc.check(true,false,true); }catch(e){out.eDrc=''+e;}
 g.ph='rules';
 try{ out.ruleName=await eda.pcb_Drc.getCurrentRuleConfigurationName(); }catch(e){}
 try{ out.rules=await eda.pcb_Drc.getCurrentRuleConfiguration(); }catch(e){out.eRule=''+e;}
 try{ out.netRules=await eda.pcb_Drc.getNetRules('DEFAULT'); }catch(e){out.eNR=''+e;}
 g.out=out; g.state='done'; g.ph='ok';
})();
return 'kicked';
