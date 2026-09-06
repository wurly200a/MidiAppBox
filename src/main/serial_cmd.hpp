#pragma once

namespace serialcmd {

// USB Serial/JTAG 上のコマンドコンソールを開始する。
// CONFIG_MIDIBOX_SERIAL_CMD が無効なら何もしない。
// SD の準備(launcher_prepare_sd)より後に呼ぶこと(ls / run が SD を見るため)。
void Init();

} // namespace serialcmd
