// Testes de exibicao do Nuvio .tpk (src/tpkscan.c), folha ASS performance:
//  - "Scan display APIs": a parte .NET. Reflexao, SO LEITURA, sobre os
//    assemblies do TizenFX carregados: membros com nomes de brilho, luminancia,
//    gama, plano, OSD, HDR... (inclusive os nao publicos e os [DllImport] do
//    Interop, que dizem o nome da funcao nativa por tras). Nada e chamado.
//  - "Window brightness (test)": Window.SetBrightness(int) por reflexao, se o
//    TizenFX desta TV o tiver, nas nossas janelas (a principal e o GLWindow).
//    Off devolve o valor que a janela tinha antes.
// So no host Tizen 6+ (Program.cs). O C pede pelas funcoes registradas em
// nv_tpk_exibe_registrar; tudo vai para o log com "[assperf]" (aba Subtitles).
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace NuvioTpk
{
    static class Exibe
    {
        [DllImport("libnuvio.so")] static extern void nv_tpk_exibe_registrar(IntPtr scan, IntPtr brilho);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FnSemArg();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FnInt(int valor);

        // Referencias vivas: o C guarda os ponteiros, o GC nao pode recolher.
        static FnSemArg fScan;
        static FnInt fBrilho;
        static Func<object[]> janelas;
        static readonly Dictionary<object, int> original = new Dictionary<object, int>();

        public static void Registrar(Action<Action> post, Func<object[]> windows)
        {
            janelas = windows;
            fScan = () => Task.Run(() => Scan());
            fBrilho = v => post(() => Brilho(v));
            nv_tpk_exibe_registrar(Marshal.GetFunctionPointerForDelegate(fScan),
                                   Marshal.GetFunctionPointerForDelegate(fBrilho));
        }

        static readonly string[] Chaves = {
            "bright", "lumin", "gamma", "contrast", "picture", "hdr", "tonemap", "colorspace", "colourspace",
            "plane", "osd", "overlay", "backlight", "dimming", "whitebalance", "videohole", "video_hole", "opaque",
            "transparen", "pq_", "gfxplane", "graphicplane"
        };

        static readonly string[] Assemblies = {
            "Tizen.NUI", "Tizen.TV.NUI", "Tizen.Multimedia", "Tizen.TV.Multimedia", "Tizen.System",
            "Tizen.System.Information", "Tizen.TV", "Tizen.TV.Application.Window", "Tizen.Applications.Common", "ElmSharp"
        };

        static void Scan()
        {
            int linhas = 0;
            try
            {
                var lista = new List<Assembly>(AppDomain.CurrentDomain.GetAssemblies());
                foreach (var nome in Assemblies)
                {
                    try { lista.Add(Assembly.Load(new AssemblyName(nome))); }
                    catch (Exception e) { Log(".net " + nome + ": not loadable (" + e.GetType().Name + ")"); }
                }
                var vistos = new HashSet<string>();
                foreach (var a in lista)
                {
                    string an = a.GetName().Name ?? "";
                    if (!(an.StartsWith("Tizen") || an.StartsWith("ElmSharp"))) continue;
                    if (!vistos.Add(an)) continue;
                    Type[] tipos;
                    try { tipos = a.GetTypes(); }
                    catch (ReflectionTypeLoadException e) { tipos = e.Types; }
                    catch (Exception e) { Log(".net " + an + ": types unreadable (" + e.GetType().Name + ")"); continue; }
                    int achou = 0;
                    foreach (var t in tipos)
                    {
                        if (t == null) continue;
                        MemberInfo[] membros;
                        try
                        {
                            membros = t.GetMembers(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static |
                                                   BindingFlags.Instance | BindingFlags.DeclaredOnly);
                        }
                        catch { continue; }
                        foreach (var m in membros)
                        {
                            if (!Casa(m.Name)) continue;
                            achou++;
                            if (linhas < 400) { Log(".net " + an + " " + t.FullName + "." + Descreve(m)); linhas++; }
                        }
                    }
                    Log(".net " + an + " " + (a.GetName().Version != null ? a.GetName().Version.ToString() : "") + ": " +
                        achou + " display-related member(s)");
                }
            }
            catch (Exception e) { Log(".net scan failed: " + e.GetType().Name + ": " + e.Message); }
            Log(".net done (" + linhas + " member lines)");
        }

        static bool Casa(string nome)
        {
            string n = nome.ToLowerInvariant();
            foreach (var k in Chaves) if (n.Contains(k)) return true;
            return false;
        }

        static string Descreve(MemberInfo m)
        {
            var mi = m as MethodInfo;
            if (mi != null)
            {
                var s = new StringBuilder();
                var ps = mi.GetParameters();
                for (int i = 0; i < ps.Length; i++) { if (i > 0) s.Append(", "); s.Append(ps[i].ParameterType.Name); }
                string nativo = "";
                if ((mi.Attributes & MethodAttributes.PinvokeImpl) != 0)
                {
                    nativo = " [native]";
                    try
                    {
                        var di = mi.GetCustomAttribute<DllImportAttribute>();
                        if (di != null) nativo = " [native " + di.Value + " " + (di.EntryPoint ?? mi.Name) + "]";
                    }
                    catch { }
                }
                return mi.Name + "(" + s + ") : " + mi.ReturnType.Name + (mi.IsPublic ? "" : " (non-public)") + nativo;
            }
            var pi = m as PropertyInfo;
            if (pi != null) return pi.Name + " { property " + pi.PropertyType.Name + " }";
            var fi = m as FieldInfo;
            if (fi != null) return fi.Name + " (field " + fi.FieldType.Name + ")";
            return m.Name + " (" + m.MemberType + ")";
        }

        // Thread principal (post). v: 100 ou 30; -1 = devolver o valor de antes.
        static void Brilho(int v)
        {
            object[] ws;
            try { ws = janelas != null ? janelas() : new object[0]; }
            catch (Exception e) { LogB(e.GetType().Name + ": " + e.Message); return; }
            foreach (var w in ws)
            {
                if (w == null) continue;
                var t = w.GetType();
                try
                {
                    var set = t.GetMethod("SetBrightness", new[] { typeof(int) });
                    var get = t.GetMethod("GetBrightness", Type.EmptyTypes);
                    string antes = get != null ? Convert.ToString(get.Invoke(w, null)) : "n/a";
                    if (set == null)
                    {
                        LogB(t.Name + " has no SetBrightness(int) in this TizenFX (GetBrightness " +
                             (get != null ? "= " + antes : "missing") + ")");
                        continue;
                    }
                    int alvo = v;
                    if (v < 0)
                    {
                        if (!original.TryGetValue(w, out alvo)) { LogB(t.Name + ": off (never changed)"); continue; }
                        original.Remove(w);
                    }
                    else if (!original.ContainsKey(w))
                    {
                        int o;
                        if (int.TryParse(antes, out o)) original[w] = o;
                    }
                    object r = set.Invoke(w, new object[] { alvo });
                    string depois = get != null ? Convert.ToString(get.Invoke(w, null)) : "n/a";
                    LogB(t.Name + ": before " + antes + " -> SetBrightness(" + alvo + ") returned " + Convert.ToString(r) +
                         " -> now " + depois);
                }
                catch (TargetInvocationException e)
                {
                    var x = e.InnerException ?? e;
                    LogB(t.Name + ": " + x.GetType().Name + ": " + x.Message);
                }
                catch (Exception e) { LogB(t.Name + ": " + e.GetType().Name + ": " + e.Message); }
            }
        }

        static void Log(string s) { try { NvVid.LogNativo("[assperf] scan " + s); } catch { } }
        static void LogB(string s) { try { NvVid.LogNativo("[assperf] window brightness: " + s); } catch { } }
    }
}
