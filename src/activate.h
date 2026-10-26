#pragma once

#include <string>

// Brings an app forward by its AUMID ("5319275A.WhatsAppDesktop_cv1g1gvanyjgm!App").
//
// Two paths, because packaged and unpackaged apps behave differently:
//   1. IApplicationActivationManager::ActivateApplication, the documented way to
//      start or focus a packaged app.
//   2. ShellExecute on shell:AppsFolder\<AUMID>, which covers anything registered
//      in the shell namespace that the activation manager refuses.
//
// Returns false when both fail, so the caller can leave the panel alone instead
// of closing as if something happened.
bool activateAumid(const std::wstring& aumid);
