// libavoc.so FALSA para tests/tpkluz.sh: o valor do "luminance limit" mora no
// arquivo $FAKE_AVOC_DB (como o banco do avocd, sobrevive ao processo).
// $FAKE_AVOC_GET_ERR=1 faz o get falhar. Assinaturas de avoc_findings_full.md.
#include <stdio.h>
#include <stdlib.h>

static int lerDb(void) {
  FILE *f = fopen(getenv("FAKE_AVOC_DB"), "r");
  int v = -1;
  if (f) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
  return v;
}

int avoc_get_luminance_limit(int *out_value, int type) {
  if (getenv("FAKE_AVOC_GET_ERR") || type != 1) return -5;
  *out_value = lerDb();
  return 0;
}

int avoc_set_luminance_limit(int limit, int type) {
  FILE *f;
  if (type != 1 || limit < 0 || limit > 1) return -22;   // o avocd recusa fora de 0..1
  f = fopen(getenv("FAKE_AVOC_DB"), "w");
  if (!f) return -5;
  fprintf(f, "%d\n", limit);
  fclose(f);
  return 0;
}
