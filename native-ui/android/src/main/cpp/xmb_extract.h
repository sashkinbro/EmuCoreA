// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace xmb {

// Builds the XMB resource tree (images/audio/models/fonts/rco/text/manifest.json)
// from a flash0 directory, the same output as native-ui/tools/extract_psp_resources.py.
// Writes into out_dir (created if needed). Returns "" on success, otherwise a
// short English error. Never touches the emulation core; safe on any thread.
std::string ExtractResources(const std::string &flash0_dir, const std::string &out_dir);

}  // namespace xmb
