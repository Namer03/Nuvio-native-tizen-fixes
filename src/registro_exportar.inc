// O PAINEL "EXPORTAR REGISTRO" (Ajustes > Sobre e ajuda > Exportar registro):
// ilha modal, irma do painel de envio (registro_envio.inc), mas SEM internet e
// SEM servidor nosso: a TV sobe o servidor de uso unico da LAN (celular.h), mostra
// o QR, e o celular baixa o arquivo. O texto sai ja limpo (logexport.h: URLs
// cortadas no host, chaves e e-mails ocultados), porque o http da LAN nao e
// cifrado. Incluido por registro.c.

static int exAberto, exErro;   // exErro: 0 = no ar, 1 = nada para exportar
static long exTam;

int registro_exportar_aberto(void) { return exAberto; }

#if defined(NV_TPK) || defined(NV_ANDROID)
#define EX_ANTERIOR (getenv("NUVIO_LOG_ANTERIOR"))
#else
#define EX_ANTERIOR "/tmp/nuvio-anterior.log"
#endif
#if defined(__EMSCRIPTEN__)
#define EX_PLAT "tizen"
#elif defined(NV_TPK)
#define EX_PLAT "tizen-tpk"
#elif defined(NV_ANDROID)
#define EX_PLAT "android"
#elif defined(NV_LINUX_DESKTOP)
#define EX_PLAT "linux-desktop"
#elif defined(__APPLE__)
#define EX_PLAT "mac"
#else
#define EX_PLAT "webos"
#endif

// Monta o texto limpo e sobe o servidor. O servidor guarda a PROPRIA copia.
static void exSubir(void) {
  size_t n = 0;
  char etapas[320] = "", *t;
  const char *dados = getenv("NUVIO_DADOS");
  celular_fechar();
  exErro = 0; exTam = 0;
  if (dados && dados[0]) snprintf(etapas, sizeof etapas, "%s/tpk-etapas.txt", dados);
  t = logexport_montar(registro_arquivo(), EX_ANTERIOR, etapas, NV_VERSAO, EX_PLAT, &n);
  if (!t) { exErro = 1; return; }
  exTam = (long)n;
  celular_exportar_abrir("Exportar registro", t, n);   // sem rede: o estado diz (CEL_PARADO)
  memset(t, 0, n);
  free(t);
}

void registro_exportar_abrir(void) {
  exAberto = 1;
  fflush(stdout);   // o que o app ja escreveu entra no arquivo
  exSubir();
}

static void exFechar(void) { exAberto = 0; celular_fechar(); }

int registro_exportar_evento(const SDL_Event *e) {
  SDL_Keycode k;
  if (!exAberto) return 0;
  if (e->type == SDL_KEYUP &&
      ((soltarSym && e->key.keysym.sym == soltarSym) ||
       (soltarScan >= 0 && (int)e->key.keysym.scancode == soltarScan))) {
    soltarSym = 0; soltarScan = -1;
    return 1;
  }
  if (e->type != SDL_KEYDOWN) return e->type == SDL_KEYUP || e->type == SDL_TEXTINPUT;
  k = e->key.keysym.sym;
  if (nv_tecla_voltar(e)) { exFechar(); engolirSoltura(e); return 1; }
  if (ehOk(k)) {
    // No ar: OK fecha. Baixado: fecha. Expirou/falhou/sem rede/sem registro: de novo.
    if (!exErro && celular_estado() == CEL_ESPERANDO) exFechar();
    else if (!exErro && celular_estado() == CEL_RECEBIDO) exFechar();
    else exSubir();
    engolirSoltura(e);
  }
  return 1;
}

static void registro_exportar_desenharCorpo_(void);
void registro_exportar_desenhar(void) {
  ESCALA_INI();
  registro_exportar_desenharCorpo_();
  ESCALA_FIM();
}

static void registro_exportar_desenharCorpo_(void) {
  float x, y, w, base = RE_Y + RE_H - 56;
  float qx = RE_X + 60, qy = RE_Y + 56, qw = 400, qh = 452, ql = celb_qr_lado();
  char kb[32], v[200];
  int est, tentar;
  const char *kick = "Sobre e ajuda", *tit, *txt = NULL;
  if (!exAberto) return;
  ponteiro_camada();   // modal sobre os Ajustes: os alvos de tras deixam de valer
  est = celular_estado();
  ajustes_ui_ilha((GfxRect){ RE_X, RE_Y, RE_W, RE_H }, 36, 1);

  // Esquerda: o QR (ou o icone do estado) numa caixa.
  rgNeutro((GfxRect){ qx, qy, qw, qh }, 24, 0.07f, RG_CHIP_SOL, 1);
  if (!exErro && est == CEL_ESPERANDO)
    celb_qr_desenhar(celular_url(), qx + (qw - ql) * 0.5f, qy + (qh - ql) * 0.5f, 1);
  else if (!exErro && est == CEL_RECEBIDO)
    rgIconeCor("aj_check", qx + (qw - 64) * 0.5f, qy + (qh - 64) * 0.5f, 64, RG_VERDE, 1);
  else
    rgIcone("aj_file-text", qx + (qw - 64) * 0.5f, qy + (qh - 64) * 0.5f, 64, 0.35f);

  x = qx + qw + 60; w = RE_X + RE_W - 60 - x; y = qy;
  if (exErro) { tit = "Sem registro para exportar"; }
  else if (est == CEL_RECEBIDO) { tit = "Registro baixado"; }
  else if (est == CEL_ESPERANDO) { tit = "Exportar registro"; txt = "Aponte a câmera do celular. Mesma rede Wi-Fi, vale por 5 minutos."; }
  else {
    tit = "Exportar registro";
    txt = est == CEL_FALHOU ? "Endereço bloqueado por tentativas erradas. Aperte OK para gerar outro."
        : est == CEL_EXPIROU ? "O endereço expirou. Aperte OK para gerar outro."
        : "Sem rede local. Conecte a TV ao Wi-Fi e aperte OK.";
  }
  ajustes_ui_kicker(kick, x, y, 1);
  y += 18 + 8;
  y += rgBloco(TXT_ILHA_TITULO, tit, x, y, w, 48, 1, 2) + 14;
  if (txt) y += rgBloco(TXT_AJ_TEXTO, txt, x, y, w, 30, 0.62f, 3) + 14;
  y += rgBloco(TXT_AJ_TEXTO, "Salva o registro no celular por QR, na mesma rede Wi-Fi. URLs, chaves e e-mails vão ocultados.",
               x, y, w, 30, 0.45f, 3) + 22;
  if (!exErro) {
    rgTamTexto(kb, sizeof kb, exTam);
    y += rgStat(i18n("Tamanho"), kb, x, y, w);
    if (est == CEL_ESPERANDO) {
      snprintf(v, sizeof v, "%s", celular_url());
      rgStat(i18n("Endereço"), v, x, y, w);
    }
  }
  tentar = exErro || (est != CEL_ESPERANDO && est != CEL_RECEBIDO);
  rgBotao(tentar ? "Tentar de novo" : "Fechar", tentar ? "aj_rotate-cw" : NULL, x, base - 56, 1, 1);
  rgKbdTexto("Voltar", "Fechar", x + w, base - 28);
}
