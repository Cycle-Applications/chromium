// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "apps/app_shim/app_shim_host_mac.h"

#include "apps/app_shim/app_shim_handler_mac.h"
#include "apps/app_shim/app_shim_messages.h"
#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "content/public/browser/browser_thread.h"
#include "ipc/ipc_channel_proxy.h"

AppShimHost::AppShimHost() {}

AppShimHost::~AppShimHost() {
  DCHECK(CalledOnValidThread());
  apps::AppShimHandler* handler = apps::AppShimHandler::GetForAppMode(app_id_);
  if (handler)
    handler->OnShimClose(this);
}

void AppShimHost::ServeChannel(const IPC::ChannelHandle& handle) {
  DCHECK(CalledOnValidThread());
  DCHECK(!channel_.get());
  channel_.reset(new IPC::ChannelProxy(
      handle,
      IPC::Channel::MODE_SERVER,
      this,
      content::BrowserThread::GetMessageLoopProxyForThread(
          content::BrowserThread::IO).get()));
}

base::FilePath AppShimHost::GetProfilePath() const {
  return profile_path_;
}

std::string AppShimHost::GetAppId() const {
  return app_id_;
}

bool AppShimHost::OnMessageReceived(const IPC::Message& message) {
  DCHECK(CalledOnValidThread());
  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP(AppShimHost, message)
    IPC_MESSAGE_HANDLER(AppShimHostMsg_LaunchApp, OnLaunchApp)
    IPC_MESSAGE_HANDLER(AppShimHostMsg_FocusApp, OnFocus)
    IPC_MESSAGE_HANDLER(AppShimHostMsg_QuitApp, OnQuit)
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()

  return handled;
}

void AppShimHost::OnChannelError() {
  Close();
}

bool AppShimHost::Send(IPC::Message* message) {
  DCHECK(channel_.get());
  return channel_->Send(message);
}

void AppShimHost::OnLaunchApp(base::FilePath profile_dir,
                              std::string app_id,
                              apps::AppShimLaunchType launch_type) {
  DCHECK(CalledOnValidThread());
  DCHECK(profile_path_.empty());
  if (!profile_path_.empty()) {
    // Only one app launch message per channel.
    Send(new AppShimMsg_LaunchApp_Done(false));
    return;
  }

  profile_path_ = profile_dir;
  app_id_ = app_id;

  apps::AppShimHandler* handler = apps::AppShimHandler::GetForAppMode(app_id_);
  bool success = handler && handler->OnShimLaunch(this, launch_type);
  Send(new AppShimMsg_LaunchApp_Done(success));
}

void AppShimHost::OnFocus() {
  DCHECK(CalledOnValidThread());
  apps::AppShimHandler* handler = apps::AppShimHandler::GetForAppMode(app_id_);
  if (handler)
    handler->OnShimFocus(this);
}

void AppShimHost::OnQuit() {
  DCHECK(CalledOnValidThread());
  apps::AppShimHandler* handler = apps::AppShimHandler::GetForAppMode(app_id_);
  if (handler)
    handler->OnShimQuit(this);
}

void AppShimHost::OnAppClosed() {
  Close();
}

void AppShimHost::Close() {
  DCHECK(CalledOnValidThread());
  delete this;
}
