// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/chromeos/login/kiosk_enable_screen_handler.h"

#include <string>

#include "chrome/browser/browser_process.h"
#include "chrome/common/chrome_notification_types.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_service.h"
#include "grit/browser_resources.h"
#include "grit/generated_resources.h"

namespace {

// Reset screen id.
const char kKioskEnableScreen[] = "kiosk-enable";

}  // namespace

namespace chromeos {

KioskEnableScreenHandler::KioskEnableScreenHandler()
    : delegate_(NULL),
      show_on_init_(false),
      is_configurable_(false),
      weak_ptr_factory_(this) {
}

KioskEnableScreenHandler::~KioskEnableScreenHandler() {
  if (delegate_)
    delegate_->OnActorDestroyed(this);
}

void KioskEnableScreenHandler::Show() {
  if (!page_is_ready()) {
    show_on_init_ = true;
    return;
  }

  KioskAppManager::Get()->GetConsumerKioskModeStatus(
      base::Bind(&KioskEnableScreenHandler::OnGetConsumerKioskModeStatus,
                 weak_ptr_factory_.GetWeakPtr()));
}

void KioskEnableScreenHandler::OnGetConsumerKioskModeStatus(
    KioskAppManager::ConsumerKioskModeStatus status) {
  is_configurable_ =
      (status == KioskAppManager::CONSUMER_KIOSK_MODE_CONFIGURABLE);
  if (!is_configurable_) {
    LOG(WARNING) << "Consumer kiosk feature is not configurable anymore!";
    return;
  }

  ShowScreen(kKioskEnableScreen, NULL);

  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_KIOSK_ENABLE_WARNING_VISIBLE,
      content::NotificationService::AllSources(),
      content::NotificationService::NoDetails());
}

void KioskEnableScreenHandler::SetDelegate(Delegate* delegate) {
  delegate_ = delegate;
  if (page_is_ready())
    Initialize();
}

void KioskEnableScreenHandler::DeclareLocalizedValues(
    LocalizedValuesBuilder* builder) {
  builder->Add("kioskEnableTitle", IDS_KIOSK_ENABLE_SCREEN_WARNING);
  builder->Add("kioskEnableWarningText",
               IDS_KIOSK_ENABLE_SCREEN_WARNING);
  builder->Add("kioskEnableWarningDetails",
               IDS_KIOSK_ENABLE_SCREEN_WARNING_DETAILS);
  builder->Add("kioskEnableButton", IDS_KIOSK_ENABLE_SCREEN_ENABLE_BUTTON);
  builder->Add("kioskCancelButton", IDS_CANCEL);
  builder->Add("kioskOKButton", IDS_OK);
  builder->Add("kioskEnableSuccessMsg", IDS_KIOSK_ENABLE_SCREEN_SUCCESS);
  builder->Add("kioskEnableErrorMsg", IDS_KIOSK_ENABLE_SCREEN_ERROR);
}

void KioskEnableScreenHandler::Initialize() {
  if (!page_is_ready() || !delegate_)
    return;

  if (show_on_init_) {
    Show();
    show_on_init_ = false;
  }
}

void KioskEnableScreenHandler::RegisterMessages() {
  AddCallback("kioskOnClose", &KioskEnableScreenHandler::HandleOnClose);
  AddCallback("kioskOnEnable", &KioskEnableScreenHandler::HandleOnEnable);
}

void KioskEnableScreenHandler::HandleOnClose() {
  if (delegate_)
    delegate_->OnExit();

  content::NotificationService::current()->Notify(
      chrome::NOTIFICATION_KIOSK_ENABLE_WARNING_COMPLETED,
      content::NotificationService::AllSources(),
      content::NotificationService::NoDetails());
}

void KioskEnableScreenHandler::HandleOnEnable() {
  if (!is_configurable_) {
    NOTREACHED();
    if (delegate_)
      delegate_->OnExit();

    content::NotificationService::current()->Notify(
        chrome::NOTIFICATION_KIOSK_ENABLE_WARNING_COMPLETED,
        content::NotificationService::AllSources(),
        content::NotificationService::NoDetails());
    return;
  }

  KioskAppManager::Get()->EnableConsumerModeKiosk(
      base::Bind(&KioskEnableScreenHandler::OnEnableConsumerModeKiosk,
                 weak_ptr_factory_.GetWeakPtr()));
}

void KioskEnableScreenHandler::OnEnableConsumerModeKiosk(bool success) {
  if (!success)
    LOG(WARNING) << "Consumer kiosk mode can't be enabled!";

  base::FundamentalValue value(success);
  web_ui()->CallJavascriptFunction("login.KioskEnableScreen.onCompleted",
                                   value);
  if (success) {
    content::NotificationService::current()->Notify(
        chrome::NOTIFICATION_KIOSK_ENABLED,
        content::NotificationService::AllSources(),
        content::NotificationService::NoDetails());
  }
}

}  // namespace chromeos
