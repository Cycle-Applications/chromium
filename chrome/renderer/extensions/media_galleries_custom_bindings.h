// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_EXTENSIONS_MEDIA_GALLERIES_CUSTOM_BINDINGS_H_
#define CHROME_RENDERER_EXTENSIONS_MEDIA_GALLERIES_CUSTOM_BINDINGS_H_

#include "chrome/renderer/extensions/chrome_v8_extension.h"

namespace extensions {

// Implements custom bindings for the media galleries API.
class MediaGalleriesCustomBindings : public ChromeV8Extension {
 public:
  MediaGalleriesCustomBindings(Dispatcher* dispatcher,
                               ChromeV8Context* context);

 private:
  // FileSystemObject GetMediaFileSystem(string file_system_url): construct
  // a file system object from a file system url.
  void GetMediaFileSystemObject(
      const v8::FunctionCallbackInfo<v8::Value>& args);

  // DirectoryReader GetMediaFileSystem(FileEntry): synchronously return a
  // directory reader for a virtual directory.  The directory will contain
  // all of the thumbnails embedded in the passed file.
  void ExtractEmbeddedThumbnails(
      const v8::FunctionCallbackInfo<v8::Value>& args);

  DISALLOW_COPY_AND_ASSIGN(MediaGalleriesCustomBindings);
};

}  // namespace extensions

#endif  // CHROME_RENDERER_EXTENSIONS_MEDIA_GALLERIES_CUSTOM_BINDINGS_H_
