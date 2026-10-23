// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace tc3::preview {
// GS thread only. Retain the deinterlaced image before any optional effects,
// so repeated edits never sharpen/filter an already processed preview.
void Capture();
void Prepare();
void Reset();
}
