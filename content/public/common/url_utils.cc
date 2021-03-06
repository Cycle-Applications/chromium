// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/common/url_utils.h"

#include "build/build_config.h"
#include "content/common/savable_url_schemes.h"
#include "content/public/common/url_constants.h"
#include "googleurl/src/gurl.h"

namespace content {

const char* const* GetSavableSchemes() {
  return GetSavableSchemesInternal();
}

bool HasWebUIScheme(const GURL& url) {
  return
#if !defined(OS_IOS)
         url.SchemeIs(chrome::kChromeDevToolsScheme) ||
         url.SchemeIs(chrome::kChromeInternalScheme) ||
#endif
         url.SchemeIs(chrome::kChromeUIScheme);
}

bool IsSavableURL(const GURL& url) {
  for (int i = 0; GetSavableSchemes()[i] != NULL; ++i) {
    if (url.SchemeIs(GetSavableSchemes()[i]))
      return true;
  }
  return false;
}

}  // namespace content
