#!/usr/bin/env python3
"""The contract the plugin SERVES must be the one module.json DECLARES.

    build/test_tuner --dump-contract > build/served_contract.txt
    tests/check_contract.py build/served_contract.txt

Also checks the hierarchy against chain_params: every key a level names is
declared, every declared param is reachable (a level names it, or a canvas
page reads it through extra_keys), and no level maps more than the eight
physical knobs. And the module.json fields the chain host relies on.
"""

import json
import sys

served = open(sys.argv[1]).read().split("\n")
module = json.load(open("src/module.json"))
caps = module["capabilities"]
ok = True


def fail(msg):
    global ok
    print("  FAIL " + msg)
    ok = False


if json.loads(served[0]) != caps["chain_params"]:
    fail("chain_params served by the plugin differ from module.json")
if json.loads(served[1]) != caps["ui_hierarchy"]:
    fail("ui_hierarchy served by the plugin differs from module.json")

params = {p["key"]: p for p in caps["chain_params"]}
named = set()
for lvl in caps["ui_hierarchy"]["levels"].values():
    named.update(lvl.get("knobs", []))
    for p in lvl.get("params", []):
        if isinstance(p, dict) and "key" in p:
            named.add(p["key"])
extra = set()
for p in params.values():
    extra.update(p.get("extra_keys", []))
    extra.update((p.get("viz") or {}).get("extra_keys", []) if isinstance(p.get("viz"), dict) else [])

missing = (named | extra) - set(params)
unreached = set(params) - named - extra
if missing:
    fail("hierarchy names undeclared keys: %s" % sorted(missing))
if unreached:
    fail("declared but unreachable: %s" % sorted(unreached))
for name, lvl in caps["ui_hierarchy"]["levels"].items():
    if len(lvl.get("knobs", [])) > 8:
        fail("level %s maps more than 8 knobs" % name)
for p in params.values():
    if len(p.get("extra_keys", [])) > 4:
        fail("%s asks for more than four extra_keys; the host caps it" % p["key"])

# The reading must stay a live, read-only, custom-drawn key: losing any of the
# three degrades it silently (no refresh, a turnable cell, or a plain dial).
t = params.get("tuner")
if not t or t.get("access") != "read" or t.get("live") is not True or \
        (t.get("viz") or {}).get("kind") != "custom:tuner":
    fail("tuner must be access:read, live:true, viz.kind custom:tuner")
if (params.get("level") or {}).get("access") != "read":
    fail("level must be access:read")

# Mute defaults to Off: the module passes audio through until asked not to.
m = params.get("mute")
if not m or m.get("default") != 0 or m.get("options") != ["Off", "On"]:
    fail("mute must be an Off/On enum defaulting to Off")

# A slot's chain opens modules/audio_fx/<id>/<id>.so whatever module.json
# says, and Master FX / sends open "dsp" -- so the two must agree.
if module.get("dsp") != module["id"] + ".so":
    fail('"dsp" must be "<id>.so" (%r)' % module.get("dsp"))
if module.get("component_type") != "audio_fx" or caps.get("component_type") != "audio_fx":
    fail("component_type must be audio_fx, top level and in capabilities")
if caps.get("chainable") is not True:
    fail("capabilities.chainable must be true")
if module.get("api_version") != 2:
    fail("api_version must be 2")
if len(json.dumps(module)) > 8192:
    fail("module.json is over the loader's 8 KB cap")

print("  contract OK" if ok else "")
sys.exit(0 if ok else 1)
