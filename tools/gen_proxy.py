"""Generate the WINHTTP proxy's export table from this PC's System32\winhttp.dll.
    py -3.12 gen_proxy.py   writes src\winhttp.def, src\winhttp_stubs.asm, src\winhttp_names.inc
Each export is a one-instruction jump to the real function, resolved when the DLL loads."""
import os, pefile
here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "..", "src")
pe = pefile.PE(r"C:\Windows\System32\winhttp.dll", fast_load=True)
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
syms = [s for s in pe.DIRECTORY_ENTRY_EXPORT.symbols]
named = [s.name.decode() for s in syms if s.name]
print(len(syms), "exports,", len(named), "named", "unnamed ordinals", [s.ordinal for s in syms if not s.name][:20])
with open(os.path.join(src, "winhttp.def"), "w", newline="\n") as f:
    f.write("LIBRARY winhttp\nEXPORTS\n")
    for n in named:
        f.write(f"  {n}={n}_stub\n")
with open(os.path.join(src, "winhttp_stubs.asm"), "w", newline="\n") as f:
    f.write("EXTERN g_realWinhttp:QWORD\n.code\n")
    for i, n in enumerate(named):
        f.write(f"{n}_stub PROC\n  jmp QWORD PTR [g_realWinhttp + {8 * i}]\n{n}_stub ENDP\n")
    f.write("END\n")
with open(os.path.join(src, "winhttp_names.inc"), "w", newline="\n") as f:
    for n in named:
        f.write(f'"{n}",\n')
