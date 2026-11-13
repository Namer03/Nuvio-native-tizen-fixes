/* ORIGINAL AUDIO WITH TORBOX BURN-IN (owner 13/11/2026). While TorBox's burned
 * picture plays (tbburn.h), the sound comes from the original file, read by
 * Nuvio on ONE sequential connection (dts_engine_open_demux), instead of
 * TorBox's stereo downmix. A local HTTP server on 127.0.0.1 (one session at a
 * time) serves the TV player one of two deliveries:
 *
 *   PARALLEL (default): a master playlist whose video is TorBox's own media
 *     playlist (mirrored with absolute URLs: the TV downloads the video from
 *     TorBox itself) and whose audio is one HLS audio rendition per audio
 *     track of the file (#EXT-X-MEDIA TYPE=AUDIO), made here segment by
 *     segment on TorBox's timeline. No video passes through Nuvio.
 *   REMUX (fallback, Settings / Audio menu): one stream. Nuvio downloads
 *     TorBox segment k, keeps its video packets and muxes in the original
 *     audio of the same time window (MPEG-TS). On any failure the TorBox
 *     segment goes through unchanged with its own audio (logged).
 *
 * Audio by codec: AAC / AC3 / E-AC3 / Opus / MP3 copied into MPEG-TS; DTS,
 * TrueHD/MLP converted to AC3 (5.1 640 kb/s, 2.0 192 kb/s); FLAC copied as is
 * into fragmented-MP4 audio segments (MPEG-TS has no FLAC) or, with the
 * Audio-menu test row "FLAC: Convert", converted (AC3 5.1 / AAC 2.0). In
 * REMUX the output is MPEG-TS only, so FLAC is always converted there.
 *
 * Connections: the original file 1 (this module), plus in REMUX one TorBox
 * segment download at a time; every generation is serialized. Nothing here
 * logs a URL (TorBox's carry its token).
 *
 * Only the .tpk with FFmpeg (NV_DTS_FFMPEG) has the server; elsewhere the
 * calls are stubs (tba_disponivel() == 0). The playlist helpers are pure and
 * compile everywhere (tests/tbaudio.sh). */
#ifndef NV_TBAUDIO_TPK_H
#define NV_TBAUDIO_TPK_H
#include <stddef.h>

enum { TBA_PARALELO = 0, TBA_REMUX = 1 };

int  tba_disponivel(void);
/* New session (replaces any previous one). tb_m3u8: TorBox's playlist URL (a
 * secret). orig/cab: the original file and the add-on's raw header lines.
 * audio_ordinal: the file's audio track (0-based among audio tracks, container
 * order). Writes the URL the TV player opens. 0 = cannot (no FFmpeg, no
 * server); the caller then plays TorBox's URL as before. */
int  tba_abrir(const char *tb_m3u8, const char *orig, const char *cab, int audio_ordinal,
               int modo, int flac_converter, char *out, size_t n);
void tba_fechar(void);
int  tba_ativo(void);
int  tba_modo(void);
/* The session failed to prepare (TorBox playlist, original file): 1 with the
 * reason (no URL). The player's open fails too; video_tpk.c falls back. */
int  tba_falhou(char *motivo, size_t n);
/* 1 once the playlists are built (tracks known). */
int  tba_pronto(void);
/* PARALLEL: renditions offered (= the TV player's audio list, in order) and
 * the rendition of a file audio track (-1 = not offered). */
int  tba_n_rendicoes(void);
int  tba_rendicao_de(int audio_ordinal);
/* The track the next playlist/segments use (DEFAULT=YES rendition / remux). */
void tba_definir_audio(int audio_ordinal);
int  tba_audio(void);
/* FLAC test row (session-wide, first build): 1 = convert. */
void tba_definir_flac(int converter);
int  tba_flac_converter(void);
/* 1 = the file has a FLAC audio track (the FLAC row shows). */
int  tba_tem_flac(void);
/* 1 = the track the session plays is FLAC. */
int  tba_audio_e_flac(void);
/* A summary line for the 10 s log (counts, timings, A/V gap). */
void tba_log(void);

/* S2 (logging only): sockets to TorBox's hosts in /proc/net/tcp(6),
 * ESTABLISHED, including the TV player's. tba_sockets_vigiar resolves the
 * hosts of up to 4 URLs in a thread; tba_sockets_contar: total, *nuvio = this
 * process's own (by inode in /proc/self/fd). -1 = unreadable / not resolved. */
void tba_sockets_vigiar(const char *u1, const char *u2, const char *u3, const char *u4);
int  tba_sockets_contar(int *nuvio);

/* ---- pure (tests/tbaudio.sh) ---------------------------------------------- */
/* `rel` against `base` (http(s) URL): absolute, root-relative, relative. */
int  tba_url_absoluta(const char *base, const char *rel, char *out, size_t n);
typedef struct { char *uri; double dur; } TbaSeg;
typedef struct {
  int    master;           /* 1: a master playlist; `variante` is its best variant */
  char   variante[4096];
  long   banda;            /* BANDWIDTH of that variant (0 unknown) */
  char   resolucao[24];
  TbaSeg *seg;
  int    nseg, fim, versao;
  char   mapa[4096];       /* EXT-X-MAP URI, absolute ("" none) */
  double alvo;             /* TARGETDURATION */
} TbaPl;
/* Parses `texto` (base = its final URL). Segment URIs come out absolute. 1 ok. */
int  tba_pl_ler(const char *texto, const char *base, TbaPl *pl);
void tba_pl_liberar(TbaPl *pl);
/* S3: the last segment's length so the total matches `dur_arquivo` (0 = keep).
 * Returns the corrected length, or the original when it does not apply. */
double tba_pl_ultimo(const TbaPl *pl, double dur_arquivo);
/* TorBox's media playlist rewritten: every URI absolute (lines and URI="")
 * against `base`, the last #EXTINF set to `ultimo` (< 0 keep). malloc'd. */
char *tba_pl_espelho(const char *texto, const char *base, double ultimo);
/* First PTS (seconds) of the first video PES in MPEG-TS bytes. 1 found. */
int  tba_ts_primeiro_pts(const unsigned char *b, size_t n, double *s);
/* fMP4: timescale of the first video track in an init segment (moov), and
 * baseMediaDecodeTime of the first traf in a media segment. 1 found. */
int  tba_mp4_escala(const unsigned char *b, size_t n, unsigned *escala);
int  tba_mp4_tfdt(const unsigned char *b, size_t n, unsigned long long *t);
#endif
