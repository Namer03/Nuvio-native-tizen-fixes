// Teste de exibicao do Nuvio .tpk (src/tpkscan.c), folha ASS performance:
// "Scan display APIs", a parte .NET. Reflexao, SO LEITURA, sobre os
// assemblies do TizenFX carregados: membros com nomes de brilho, luminancia,
// gama, plano, OSD, HDR... (inclusive os nao publicos e os [DllImport] do
// Interop, que dizem o nome da funcao nativa por tras). Nada disso e chamado.
// Tambem lista as "aux hints" que o gerenciador de janelas aceita nas nossas
// janelas (Window.GetSupportedAuxiliaryHint*): so a lista, nenhuma e ligada.
// (O teste de Window.SetBrightness saiu: devolveu False e nada mudou, 24/10.)
// "Window hint (test)": poe UMA dica do gerenciador de janelas na janela
// principal (AddAuxiliaryHint), das que o scan de 24/10 listou com cara de
// brilho/plano; Off tira. O dono olha se o branco do cartao de teste sobe.
// So no host Tizen 6+ (Program.cs). O C pede pela funcao registrada em
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
        [DllImport("libnuvio.so")] static extern void nv_tpk_exibe_registrar(IntPtr scan, IntPtr janela);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FnSemArg();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void FnInt(int v);

        // Referencias vivas: o C guarda os ponteiros, o GC nao pode recolher.
        static FnSemArg fScan;
        static FnInt fJanela;
        static Func<object[]> janelas;

        public static void Registrar(Action<Action> post, Func<object[]> windows)
        {
            janelas = windows;
            fScan = () => { post(() => AuxHints()); Task.Run(() => Scan()); };
            fJanela = v => post(() => Dica(v));
            nv_tpk_exibe_registrar(Marshal.GetFunctionPointerForDelegate(fScan),
                                   Marshal.GetFunctionPointerForDelegate(fJanela));
        }

        // Mesma ordem da folha (assperf.c, AP_JANELA): 1.. = uma dica.
        static readonly string[,] Dicas = {
            { "wm.luminescence.mode", "1" }, { "wm.luminescence.mode", "0" }, { "wm.luminescence.mode", "2" },
            { "wm.tvs.win.avoc.control", "1" }, { "wm.video.set.pq.hint", "1" }, { "wm.hwc.window", "1" },
            { "wm.hwc.dynamic_opaque_window", "1" }, { "wm.comp.win.effect.video", "1" }
        };
        static uint dicaId;
        static bool dicaPosta;

        // Thread principal (post).
        static void Dica(int v)
        {
            object[] ws;
            try { ws = janelas != null ? janelas() : new object[0]; }
            catch (Exception e) { LogD(e.GetType().Name + ": " + e.Message); return; }
            object w = ws.Length > 0 ? ws[0] : null;
            if (w == null) { LogD("no window"); return; }
            var t = w.GetType();
            try
            {
                if (dicaPosta)
                {
                    var rem = t.GetMethod("RemoveAuxiliaryHint", new[] { typeof(uint) });
                    object r = rem != null ? rem.Invoke(w, new object[] { dicaId }) : "n/a";
                    LogD("removed hint id " + dicaId + " -> " + Convert.ToString(r));
                    dicaPosta = false;
                }
                if (v < 1 || v > Dicas.GetLength(0)) { LogD("off"); return; }
                string nome = Dicas[v - 1, 0], valor = Dicas[v - 1, 1];
                var add = t.GetMethod("AddAuxiliaryHint", new[] { typeof(string), typeof(string) });
                if (add == null) { LogD(t.Name + " has no AddAuxiliaryHint in this TizenFX"); return; }
                object id = add.Invoke(w, new object[] { nome, valor });
                dicaId = Convert.ToUInt32(id);
                dicaPosta = true;
                var get = t.GetMethod("GetAuxiliaryHintValue", new[] { typeof(uint) });
                string lido = get != null ? Convert.ToString(get.Invoke(w, new object[] { dicaId })) : "n/a";
                LogD("set " + nome + "=" + valor + " -> id " + dicaId + ", window system reads back \"" + lido + "\"");
            }
            catch (TargetInvocationException e)
            {
                var x = e.InnerException ?? e;
                LogD(x.GetType().Name + ": " + x.Message);
            }
            catch (Exception e) { LogD(e.GetType().Name + ": " + e.Message); }
        }
        static void LogD(string s) { try { NvVid.LogNativo("[assperf] window hint: " + s); } catch { } }

        // Thread principal (post): as aux hints de cada janela nossa.
        static void AuxHints()
        {
            object[] ws;
            try { ws = janelas != null ? janelas() : new object[0]; }
            catch (Exception e) { Log("aux hints: " + e.GetType().Name + ": " + e.Message); return; }
            foreach (var w in ws)
            {
                if (w == null) continue;
                var t = w.GetType();
                try
                {
                    var cnt = t.GetMethod("GetSupportedAuxiliaryHintCount", Type.EmptyTypes);
                    var um = t.GetMethod("GetSupportedAuxiliaryHint", new[] { typeof(uint) });
                    var lista = t.GetMethod("GetSupportedAuxiliaryHints", Type.EmptyTypes);
                    var nomes = new List<string>();
                    if (cnt != null && um != null)
                    {
                        uint n = Convert.ToUInt32(cnt.Invoke(w, null));
                        for (uint i = 0; i < n && i < 512; i++) nomes.Add(Convert.ToString(um.Invoke(w, new object[] { i })));
                    }
                    else if (lista != null)
                    {
                        var r = lista.Invoke(w, null) as System.Collections.IEnumerable;
                        if (r != null) foreach (var x in r) nomes.Add(Convert.ToString(x));
                    }
                    else { Log("aux hints " + t.Name + ": no GetSupportedAuxiliaryHint* in this TizenFX"); continue; }
                    Log("aux hints " + t.Name + ": " + nomes.Count + " supported");
                    foreach (var h in nomes) Log("aux hint " + t.Name + ": " + h);
                }
                catch (TargetInvocationException e)
                {
                    var x = e.InnerException ?? e;
                    Log("aux hints " + t.Name + ": " + x.GetType().Name + ": " + x.Message);
                }
                catch (Exception e) { Log("aux hints " + t.Name + ": " + e.GetType().Name + ": " + e.Message); }
            }
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
            try { Completo(); } catch (Exception e) { Log(".net full dump failed: " + e.GetType().Name + ": " + e.Message); }
            Log(".net done (" + linhas + " member lines)");
        }

        // Os tipos da Samsung que poem imagem no PLANO DE VIDEO (24/10: a
        // libdali-extension-tv-photo chama Gfx_Videooutput_*): TODOS os membros,
        // com assinatura, inclusive o Interop (nome da funcao nativa).
        static readonly string[] TiposVideo = { "PhotoViewer", "VideoCanvas", "VideoView", "VideoPlane", "Canvas", "Photo" };
        static void Completo()
        {
            int n = 0;
            foreach (var nome in new[] { "Tizen.TV.NUI", "Tizen.TV.Multimedia", "Tizen.TV" })
            {
                Assembly a;
                try { a = Assembly.Load(new AssemblyName(nome)); } catch { continue; }
                Log(".net full " + nome + " from " + SafeLoc(a));
                Type[] tipos;
                try { tipos = a.GetTypes(); }
                catch (ReflectionTypeLoadException e) { tipos = e.Types; }
                foreach (var t in tipos)
                {
                    if (t == null || t.FullName == null) continue;
                    bool quer = false;
                    foreach (var k in TiposVideo) if (t.FullName.Contains(k)) quer = true;
                    if (!quer) continue;
                    Log(".net type " + t.FullName + (t.BaseType != null ? " : " + t.BaseType.FullName : ""));
                    MemberInfo[] ms;
                    try
                    {
                        ms = t.GetMembers(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static |
                                          BindingFlags.Instance | BindingFlags.DeclaredOnly);
                    }
                    catch { continue; }
                    foreach (var m in ms)
                    {
                        if (n++ > 900) { Log(".net full: stopped at 900 lines"); return; }
                        var ci = m as ConstructorInfo;
                        if (ci != null)
                        {
                            var sb = new StringBuilder();
                            foreach (var p in ci.GetParameters()) sb.Append((sb.Length > 0 ? ", " : "") + p.ParameterType.Name + " " + p.Name);
                            Log(".net   " + t.Name + "(" + sb + ")" + (ci.IsPublic ? "" : " (non-public)"));
                            continue;
                        }
                        var mi = m as MethodInfo;
                        if (mi != null)
                        {
                            var sb = new StringBuilder();
                            foreach (var p in mi.GetParameters()) sb.Append((sb.Length > 0 ? ", " : "") + p.ParameterType.Name + " " + p.Name);
                            Log(".net   " + Descreve(m).Split('(')[0] + "(" + sb + ")" + Descreve(m).Substring(Descreve(m).IndexOf(')') + 1));
                            continue;
                        }
                        Log(".net   " + Descreve(m));
                    }
                }
            }
        }
        static string SafeLoc(Assembly a) { try { return a.Location; } catch { return "?"; } }

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

        static void Log(string s) { try { NvVid.LogNativo("[assperf] scan " + s); } catch { } }
    }
}
