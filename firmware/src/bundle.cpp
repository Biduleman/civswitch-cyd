#include "bundle.h"

#include <SD.h>

#include "config.h"
#include "hw.h"

namespace bundle {

bool scanPayloads(std::vector<PayloadInfo> &out) {
  out.clear();
  hw::busForSd();
  File dir = SD.open(PAYLOADS_DIR);
  if (!dir || !dir.isDirectory()) return false;

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    if (!name.endsWith(".js")) {
      f.close();
      continue;
    }
    PayloadInfo p;
    p.name = name.substring(0, name.length() - 3);
    p.path = String(PAYLOADS_DIR) + "/" + name;
    p.size = f.size();
    p.ok = p.size <= LOADER_MAX_PAYLOAD;
    out.push_back(p);
    f.close();
  }
  dir.close();
  return true;
}

} // namespace bundle
