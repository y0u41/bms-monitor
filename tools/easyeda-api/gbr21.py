"""Pull a ManufactureData export out of the EDA sandbox as base64 and write it to disk.

Usage: python gbr21.py <kind> [filename]
  kind: gerber|pnp|bom|tp|netlist|dxf|pdf|ipc2581|pcinfo
"""
import base64, io, json, os, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "work"))
import v4

DOC = v4.DOC
OPEN = "await eda.dmt_EditorControl.openDocument('%s');\n" % DOC
OUT = os.path.join(os.path.dirname(os.path.dirname(HERE)), "out", "pcb")

FN = {"gerber": "getGerberFile", "pnp": "getPickAndPlaceFile", "bom": "getBomFile",
      "tp": "getTestPointFile", "netlist": "getNetlistFile", "dxf": "getDxfFile",
      "pdf": "getPdfFile", "ipc2581": "getIpc2581CFile", "pcinfo": "getPcbInfoFile"}

JS = OPEN + """var f=await eda.pcb_ManufactureData.%(fn)s('%(name)s');
var buf=await f.arrayBuffer();
var u=new Uint8Array(buf);
var s='';
var CH=8192;
for(var i=0;i<u.length;i+=CH){s+=String.fromCharCode.apply(null,u.subarray(i,i+CH));}
return {name:f.name,size:f.size,b64:btoa(s)};"""


def export(kind, name):
    js = JS % {"fn": FN[kind], "name": name}
    r = v4.call(js, t=300).get("result")
    if not isinstance(r, dict) or "b64" not in r:
        return {"error": json.dumps(r, ensure_ascii=False)[:400]}
    raw = base64.b64decode(r["b64"])
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    # f.name is whatever the API felt like naming it - it has already clobbered the
    # Gerber once when a pnp export came back calling itself "bms_a.gbr.zip".
    path = os.path.join(OUT, name)
    io.open(path, "wb").write(raw)
    out = {"path": os.path.abspath(path), "bytes": len(raw), "api_name": r.get("name")}
    if name.endswith(".zip"):
        z = zipfile.ZipFile(path)
        out["entries"] = [[n, z.getinfo(n).file_size] for n in z.namelist()]
    return out


if __name__ == "__main__":
    kind = sys.argv[1] if len(sys.argv) > 1 else "gerber"
    name = sys.argv[2] if len(sys.argv) > 2 else "bms_a.gbr.zip"
    res = export(kind, name)
    for k, v in res.get("entries", []):
        print("   %8d  %s" % (v, k))
    print(json.dumps({k: v for k, v in res.items() if k != "entries"}, ensure_ascii=False))
