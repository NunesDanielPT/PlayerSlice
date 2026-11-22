# Extrai o HLSL de src/gfx/Gfx.cpp e compila cada shader com o fxc do Windows SDK.
# Usado no GitHub Actions: o build falha se algum shader tiver erros.
import glob, os, re, subprocess, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, 'src', 'gfx', 'Gfx.cpp'), encoding='utf-8').read()
hlsl = re.search(r'R"HLSL\((.*?)\)HLSL"', src, re.S).group(1)
names = re.search(r'const char\* names\[\] = \{(.*?)\};', src, re.S).group(1)
entries = [('VS', 'vs_5_0')] + [(n, 'ps_5_0') for n in re.findall(r'"(\w+)"', names)]
path = os.path.join(root, 'build', 'shaders_check.hlsl')
os.makedirs(os.path.dirname(path), exist_ok=True)
open(path, 'w').write(hlsl)
cands = sorted(glob.glob(r'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\fxc.exe'))
if not cands:
    print('fxc.exe não encontrado'); sys.exit(1)
fxc = cands[-1]
ok = True
for e, t in entries:
    r = subprocess.run([fxc, '/nologo', '/T', t, '/E', e, '/Fo', os.devnull, path], capture_output=True, text=True)
    status = 'OK' if r.returncode == 0 else 'ERRO'
    print(f'{status:4} {e} ({t})')
    if r.returncode != 0:
        ok = False; print(r.stdout, r.stderr)
sys.exit(0 if ok else 1)
