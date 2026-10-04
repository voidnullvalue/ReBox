# Offline native-control recovery

Start with [NATIVE_CONTROL_FINDINGS.md](../../docs/NATIVE_CONTROL_FINDINGS.md). These new analysis tools read extracted regular files and invoke host inspection tools. They do not run firmware, connect to a receiver, open receiver FIFOs, mount images or deploy. Existing probes elsewhere in the repository have different behavior; none were executed in this work.

Dependencies: Python 3, Python `capstone`, host `readelf`, `c++filt` and JDK `javap`. The protocol codecs and 35 protocol tests use only Python's standard library. The five extracted-artifact recovery tests additionally need Capstone/binutils and `extracted/sdb4-rootfs`.

## Reproduce the principal inventories

From the repository root, choose a new local directory:

```sh
python3 tools/re/reproduce_native.py --output /tmp/hr54-native-evidence-review
python3 -m unittest discover -s tools/re -p 'test_*.py'
```

The reproduction script regenerates 17 files: DTVWM client/server inventories, 116 media message schemas, 406 DirectTest command comparisons, selected Java disassemblies, correlated tables and input/media routing disassemblies. Its manifest records input SHA-256 hashes, output hashes and exact host commands. It refuses an existing output directory. Additional focused disassemblies in [native-evidence](../../docs/native-evidence/README.md) can be regenerated with the address ranges embedded in their first/last instruction lines. This is not a complete firmware decompilation.

## Inspection tools

| Tool | Purpose / boundaries |
|---|---|
| [mips_elf.py](mips_elf.py) | ELF32 big-endian loads, symbols/demangling, GOT call annotations, relocation-aware vtables, explicit switch-table reads, string xrefs and imported-function callers |
| [dtvwm_inventory.py](dtvwm_inventory.py) | Cookie/request constructor recovery, including copied read-only templates; `--server` recovers two-word member-pointer handler assignments; JSON or `--markdown` |
| [car_classes.py](car_classes.py) | Build-specific CAR global/compact pools and selected class/method bodies; `--javap` reconstructs a disassembly-only JVM container |
| [car_inventory.py](car_inventory.py) | MediaPlayerProxyIPC constructors/Add-field schemas; DirectTest command comparisons and nearby attributes/calls |
| [native_reports.py](native_reports.py) | Correlates generated JSON/bytecode into DTVWM, media and raw-key Markdown tables |
| [reproduce_native.py](reproduce_native.py) | Local reproduction and source provenance |
| [test_native_protocols.py](test_native_protocols.py) | 35 independent golden-packet, malformed-input, endian, repeat and XML tests |
| [test_recovery.py](test_recovery.py) | Five extracted-file checks: all message class bounds, native/stub methods, DTVWM server/client discrepancy and relocated player rate vtable |

Examples:

```sh
python3 tools/re/mips_elf.py extracted/sdb4-rootfs/opt/key_dispatcher/lib/libuserinput.so symbols
python3 tools/re/mips_elf.py extracted/sdb4-rootfs/opt/key_dispatcher/bin/keydispatcher disasm --start 0x408de4 --end 0x408ebc
python3 tools/re/mips_elf.py extracted/sdb4-rootfs/opt/dvr_core/lib/libdvr.so tables --match 'vtable for libdvr::LocalPlayer'
python3 tools/re/mips_elf.py extracted/sdb4-rootfs/opt/itv/itvpack/root/lib/libcwebkit.so imports --match UserInput
python3 tools/re/car_classes.py extracted/sdb4-rootfs/opt/dtv/dtv.car com/directv/dvrcore/impl/MediaPlayer/MediaPlayerProxy --javap
```

`calls --match` filters enclosing **symbolized function names**; use `imports --match` to find callers of an imported symbol in stripped clients. `switch ADDRESS COUNT --gp GP` prints raw entries plus an explicitly supplied base; it does not guess relative-table semantics. Vtable recovery applies supported MIPS REL32/32 symbol relocations before naming destinations.

Linear MIPS annotations are lead-finding aids, not CFG-aware decompilation. Unknown indirect/vtable calls remain unresolved; inferred PIC function boundaries can miss leaf functions. Verify delay-slot and branch semantics in the printed instructions. DTVWM constructor lengths can be changed later or be dynamic; simple store recovery does not type the whole payload.

CAR decoding is deliberately build-specific. Every selected class must parse to its exact declared end. It handles observed compact native flags and a limited set of signature/exception trailers; annotated classes such as DvrCoreProxy remain unsupported. The `javap` container normalizes access flags and drops field declarations/interfaces/exception handlers for inspection. It must not be executed or treated as replacement firmware. DirectTest's nearby-attribute inventory is heuristic around branch boundaries; precise arguments/actions require the linked bytecode.

## Offline protocol implementations

| Tool | CLI / implemented scope |
|---|---|
| [native-input/protocol.py](../native-input/protocol.py) | `encode session/register/close/push`, `decode`; streaming event decoder and repeat tracker; exact type-0 map, not all default-registration variants |
| [dtvwm/protocol.py](../dtvwm/protocol.py) | `register`, `encode`, `decode --response`; checked surface/configuration/plane/z/bitmap-return helpers; raw regions for untyped commands |
| [native-media/protocol.py](../native-media/protocol.py) | `encode`, `decode`, `dump`; tagged BinaryIPC primitive fields via generated schema; nested serialization rejected |
| [native-uconnect/protocol.py](../native-uconnect/protocol.py) | `encode` XML, `decode` raw envelope; library request/envelope helpers; no connection bootstrap |

Each codec accepts or emits local bytes/text only. None contains a socket client or receiver executable call. Schema presence is not a claim that a server implements a message, and successful encoding does not establish ownership or hardware behavior.
