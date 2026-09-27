"""Evidence for 画板指南 §5 (the 14-item pre-order review sheet).

DRC 0 errors only proves the copper obeys the design rules; §5 exists for the five
mistakes DRC cannot see (wrong net name, missing part, mirrored silkscreen, cap on the
wrong "ground", wrong Rsns footprint).  So: dump every net's member list and the
standalone pads once, then answer each item from that dump instead of from memory.

  python review26.py dump     # refresh _pads2.json + _nets26.json
  python review26.py sheet    # the 14 items, each printed with its evidence
  python review26.py net NAME  # one net's members
  python review26.py tp        # standalone pads (test points / mounting holes)
"""
import io, json, os, sys
import v4

HERE = os.path.dirname(os.path.abspath(__file__))
NETS = os.path.join(HERE, "_nets26.json")

DUMP = "await eda.dmt_EditorControl.openDocument('%s');\n" % v4.DOC + """
var nets={};
for(const c of await eda.pcb_PrimitiveComponent.getAll()){
  var d=c.getState_Designator();
  for(const p of await eda.pcb_PrimitiveComponent.getAllPinsByPrimitiveId(c.getState_PrimitiveId())){
    var n=p.getState_Net(); if(!n) continue;
    (nets[n]=nets[n]||[]).push(d+"."+p.getState_PadNumber());
  }
}
var lone=[];
for(const p of await eda.pcb_PrimitivePad.getAll()){
  var sh=p.getState_Pad(), ho=p.getState_Hole();
  lone.push({net:p.getState_Net(),x:p.getState_X(),y:p.getState_Y(),pad:sh,hole:ho,
             layer:p.getState_Layer(),num:p.getState_PadNumber()});
}
var pours=[];
for(const q of await eda.pcb_PrimitivePour.getAll())
  pours.push({net:q.getState_Net(),layer:q.getState_Layer(),name:q.getState_PourName(),
              prio:q.getState_PourPriority(),poly:q.getState_ComplexPolygon()});
var comps=[];
for(const c of await eda.pcb_PrimitiveComponent.getAll())
  comps.push({des:c.getState_Designator(),x:c.getState_X(),y:c.getState_Y(),
              rot:c.getState_Rotation(),foot:c.getState_Footprint()});
return {nets:nets,lone:lone,pours:pours,comps:comps};"""


def dump():
    d = v4.call(DUMP, t=600)
    if not d.get("success"):
        print(json.dumps(d, ensure_ascii=False)[:900])
        return None
    r = d["result"]
    io.open(NETS, "w", encoding="utf-8").write(json.dumps(r, ensure_ascii=False))
    print("nets:%d  lone pads:%d  pours:%d  comps:%d" %
          (len(r["nets"]), len(r["lone"]), len(r["pours"]), len(r["comps"])))
    return r


def load():
    if not os.path.isfile(NETS):
        return dump()
    return json.load(io.open(NETS, encoding="utf-8"))


def members(net):
    return sorted(load()["nets"].get(net, []))


def pad_net_of(des_pin):
    d, p = des_pin.split(".")
    return {n for n, ms in load()["nets"].items() if des_pin in ms}


def show(net):
    ms = members(net)
    return "%-9s (%2d): %s" % (net, len(ms), " ".join(ms))


def sheet():
    L = load()
    nets = L["nets"]
    tp = [p for p in L["lone"]]
    print("=" * 78)
    print("1  BOOT: PE6 -> 1K -> TS1, plus 10K TS1 -> VSS")
    for n in ("AFE_BOOT", "TS1", "T0"):
        if n in nets:
            print("   " + show(n))
    print("2  ALERT: only 499K to VSS + 470pF + the line to PD13")
    print("   " + show("AFE_ALERT") if "AFE_ALERT" in nets else "   " + show("ALERT"))
    print("3  I2C pull-ups land on 3.3V, not 5V / REGOUT")
    for n in ("SCL", "SDA", "V3V3_MCU", "V5V0", "REGOUT"):
        if n in nets:
            print("   " + show(n))
    print("4/5  Rsns four terminals: 1/4 carry current, 2/3 sense")
    for n in ("PACK_N", "SW_N", "RSNS_SP", "RSNS_SN", "SRP_F", "SRN_F", "SRP", "SRN"):
        if n in nets:
            print("   " + show(n))
    print("6  VC4 tied to VC3, each with its own 100R")
    for n in ("CELL3", "CELL4"):
        if n in nets:
            print("   " + show(n))
    print("8/9  caps: REGOUT / CAP1 / Cf")
    for n in ("REGOUT", "CAP1", "VC0", "BAT"):
        if n in nets:
            print("   " + show(n))
    print("10 DSG / CHG carry nothing but the run to the B board")
    for n in ("DSG", "CHG"):
        if n in nets:
            print("   " + show(n))
    print("14 R19 is the only path between VSS and RSNS_SP")
    print("   " + show("VSS"))
    print("   " + show("RSNS_SP"))
    print("11 standalone pads (test points / mounting holes): %d" % len(tp))
    for p in tp:
        print("   (%7.1f,%7.1f) L%-3s net=%-9s pad=%s hole=%s num=%s" %
              (p["x"], p["y"], p["layer"], p["net"], p["pad"], p["hole"], p["num"]))
    print("pours:")
    for p in L["pours"]:
        print("   L%s net=%-6s name=%-9s prio=%s" % (p["layer"], p["net"], p["name"], p["prio"]))


if __name__ == "__main__":
    a = sys.argv[1:]
    m = a[0] if a else "sheet"
    if m == "dump":
        dump()
    elif m == "net":
        print(show(a[1]))
    elif m == "tp":
        for p in load()["lone"]:
            print(p)
    else:
        sheet()
