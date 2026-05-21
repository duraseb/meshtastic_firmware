#pragma once

// BirbMesh One uses the same E80-900M2213S RF switch matrix as its parent
// variant. Delegating avoids two problems the standalone copy had:
//   1. No `#pragma once` → double inclusion from InterfacesTemplates.cpp's
//      unity-style #include of LR11x0Interface.cpp + LR20x0Interface.cpp
//      caused `rfswitch_dio_pins` / `rfswitch_table` static-redefinition errors.
//   2. Missing `lr20x0_rfswitch_dio_pins` / `lr20x0_rfswitch_table` → undefined
//      symbol errors in LR20x0Interface.cpp when USE_LR2021 was set.
// The parent provides both LR11x0 and LR2021 tables; if BirbMesh ever needs a
// custom matrix that diverges from the parent, split this back out.
#include "../nrf52_promicro_diy_tcxo/rfswitch.h"
