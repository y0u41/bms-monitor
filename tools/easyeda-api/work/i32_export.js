const out={};
const tab=await eda.dmt_EditorControl.openDocument('cf8920f8e3284a24');
if(tab){await eda.dmt_EditorControl.activateDocument(tab);}
const t0=Date.now();
try{
  const f=await eda.pcb_ManufactureData.getGerberFile('bms_api.zip');
  if(!f){out.ok=false;out.msg='undefined return';}
  else{
    const buf=await f.arrayBuffer();
    const u=new Uint8Array(buf);
    let bin='';
    const CH=8192;
    for(let i=0;i<u.length;i+=CH){bin+=String.fromCharCode.apply(null,u.subarray(i,i+CH));}
    out.ok=true;out.name=f.name;out.size=u.length;out.ms=Date.now()-t0;out.b64=btoa(bin);
  }
}catch(e){out.ok=false;out.err=String(e).slice(0,300);out.ms=Date.now()-t0;}
return out;
