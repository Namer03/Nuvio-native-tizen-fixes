# 21-nvdec-live  —  LIVE read of muse-server's active decoder fd (Layer 2).
# Run this ONLY while an episode is actually playing in Nuvio.
# It ptrace-attaches to muse-server, injects READ-ONLY ioctls (G_FMT /
# G_SELECTION) on its real /dev/video61 fd, then restores registers+memory and
# detaches. It momentarily stops muse-server's main thread (like a debugger).
# muse-server is Restart=always, so the worst realistic failure is a brief A/V
# glitch, never a permanent change. Deploy first with 20-nvdec-deploy.sh.
if [ ! -x /tmp/nvdec-probe ]; then
  echo "nvdec-probe not deployed — run 20-nvdec-deploy.sh first."
  exit 1
fi
/tmp/nvdec-probe --live
