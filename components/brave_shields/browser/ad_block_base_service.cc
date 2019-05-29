/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "brave/components/brave_shields/browser/ad_block_base_service.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "brave/browser/net/url_context.h"
#include "brave/common/pref_names.h"
#include "brave/components/brave_component_updater/browser/dat_file_util.h"
#include "brave/components/brave_shields/common/brave_shield_constants.h"
#include "brave/vendor/ad-block/ad_block_client.h"
#include "brave/vendor/adblock_rust_ffi/src/wrapper.hpp"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/origin.h"

using brave_component_updater::BraveComponent;
using content::BrowserThread;
using namespace net::registry_controlled_domains;  // NOLINT

namespace {

std::string ResourceTypeToString(content::ResourceType resource_type) {
  std::string filter_option = "";
  switch (resource_type) {
    // top level page
    case content::RESOURCE_TYPE_MAIN_FRAME:
      filter_option = "main_frame";
      break;
    // frame or iframe
    case content::RESOURCE_TYPE_SUB_FRAME:
      filter_option = "sub_frame";
      break;
    // a CSS stylesheet
    case content::RESOURCE_TYPE_STYLESHEET:
      filter_option = "stylesheet";
      break;
    // an external script
    case content::RESOURCE_TYPE_SCRIPT:
      filter_option = "script";
      break;
    // an image (jpg/gif/png/etc)
    case content::RESOURCE_TYPE_FAVICON:
    case content::RESOURCE_TYPE_IMAGE:
      filter_option = "image";
      break;
    // a font
    case content::RESOURCE_TYPE_FONT_RESOURCE:
      filter_option = "font";
      break;
    // an "other" subresource.
    case content::RESOURCE_TYPE_SUB_RESOURCE:
      filter_option = "other";
      break;
    // an object (or embed) tag for a plugin.
    case content::RESOURCE_TYPE_OBJECT:
      filter_option = "object";
      break;
    // a media resource.
    case content::RESOURCE_TYPE_MEDIA:
      filter_option = "media";
      break;
    // a XMLHttpRequest
    case content::RESOURCE_TYPE_XHR:
      filter_option = "xhr";
      break;
    // a ping request for <a ping>/sendBeacon.
    case content::RESOURCE_TYPE_PING:
      filter_option = "ping";
      break;
    // the main resource of a dedicated
    case content::RESOURCE_TYPE_WORKER:
    // the main resource of a shared worker.
    case content::RESOURCE_TYPE_SHARED_WORKER:
    // an explicitly requested prefetch
    case content::RESOURCE_TYPE_PREFETCH:
    // the main resource of a service worker.
    case content::RESOURCE_TYPE_SERVICE_WORKER:
    // a report of Content Security Policy
    case content::RESOURCE_TYPE_CSP_REPORT:
    // a resource that a plugin requested.
    case content::RESOURCE_TYPE_PLUGIN_RESOURCE:
    case content::RESOURCE_TYPE_LAST_TYPE:
    default:
      break;
  }
  return filter_option;
}

FilterOption ResourceTypeToFilterOption(content::ResourceType resource_type) {
  FilterOption filter_option = FONoFilterOption;
  switch (resource_type) {
    // top level page
    case content::ResourceType::kMainFrame:
      filter_option = FODocument;
      break;
    // frame or iframe
    case content::ResourceType::kSubFrame:
      filter_option = FOSubdocument;
      break;
    // a CSS stylesheet
    case content::ResourceType::kStylesheet:
      filter_option = FOStylesheet;
      break;
    // an external script
    case content::ResourceType::kScript:
      filter_option = FOScript;
      break;
    // an image (jpg/gif/png/etc)
    case content::ResourceType::kFavicon:
    case content::ResourceType::kImage:
      filter_option = FOImage;
      break;
    // a font
    case content::ResourceType::kFontResource:
      filter_option = FOFont;
      break;
    // an "other" subresource.
    case content::ResourceType::kSubResource:
      filter_option = FOOther;
      break;
    // an object (or embed) tag for a plugin.
    case content::ResourceType::kObject:
      filter_option = FOObject;
      break;
    // a media resource.
    case content::ResourceType::kMedia:
      filter_option = FOMedia;
      break;
    // a XMLHttpRequest
    case content::ResourceType::kXhr:
      filter_option = FOXmlHttpRequest;
      break;
    // a ping request for <a ping>/sendBeacon.
    case content::ResourceType::kPing:
      filter_option = FOPing;
      break;
    // the main resource of a dedicated worker.
    case content::ResourceType::kWorker:
    // the main resource of a shared worker.
    case content::ResourceType::kSharedWorker:
    // an explicitly requested prefetch
    case content::ResourceType::kPrefetch:
    // the main resource of a service worker.
    case content::ResourceType::kServiceWorker:
    // a report of Content Security Policy violations.
    case content::ResourceType::kCspReport:
    // a resource that a plugin requested.
    case content::ResourceType::kPluginResource:
    // a service worker navigation preload request.
    case content::ResourceType::kNavigationPreload:
    // an invalid type (see brave/browser/net/url_context.h)
    case brave::BraveRequestInfo::kInvalidResourceType:
    default:
      break;
  }
  return filter_option;
}

}  // namespace

namespace brave_shields {

AdBlockBaseService::AdBlockBaseService(BraveComponent::Delegate* delegate)
    : BaseBraveShieldsService(delegate),
      ad_block_client_(new AdBlockClient()),
      weak_factory_(this),
      weak_factory_io_thread_(this),
      ad_block_client2_(new adblock::Blocker("||brianbondy.com")) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

AdBlockBaseService::~AdBlockBaseService() {
  Cleanup();
}

void AdBlockBaseService::Cleanup() {
  BrowserThread::DeleteSoon(
      BrowserThread::IO, FROM_HERE, ad_block_client_.release());
}

bool AdBlockBaseService::ShouldStartRequest(const GURL& url,
    content::ResourceType resource_type, const std::string& tab_host,
    bool* did_match_exception, bool* cancel_request_explicitly) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  FilterOption current_option = ResourceTypeToFilterOption(resource_type);

  // Determine third-party here so the library doesn't need to figure it out.
  // CreateFromNormalizedTuple is needed because SameDomainOrHost needs
  // a URL or origin and not a string to a host name.
  url::Origin tab_origin(url::Origin::CreateFromNormalizedTuple(
        "https", tab_host.c_str(), 80));
  if (SameDomainOrHost(url, tab_origin, INCLUDE_PRIVATE_REGISTRIES)) {
    current_option = static_cast<FilterOption>(
        current_option | FONotThirdParty);
  } else {
    current_option = static_cast<FilterOption>(current_option | FOThirdParty);
  }

  Filter* matching_filter = nullptr;
  Filter* matching_exception_filter = nullptr;
  if (ad_block_client2_->Matches(url.spec(),
        tab_origin.GetURL().spec(), ResourceTypeToString(resource_type))) {
    return false;
  }
  if (ad_block_client_->matches(url.spec().c_str(),
        current_option, tab_host.c_str(), &matching_filter,
        &matching_exception_filter)) {
    if (matching_filter && cancel_request_explicitly &&
        (matching_filter->filterOption & FOExplicitCancel)) {
      *cancel_request_explicitly = true;
    }
    // We'd only possibly match an exception filter if we're returning true.
    *did_match_exception = false;
    // LOG(ERROR) << "AdBlockBaseService::ShouldStartRequest(), host: "
    //  << tab_host
    //  << ", resource type: " << resource_type
    //  << ", url.spec(): " << url.spec();
    return false;
  }

  if (did_match_exception) {
    *did_match_exception = !!matching_exception_filter;
  }

  return true;
}

void AdBlockBaseService::EnableTag(const std::string& tag, bool enabled) {
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(&AdBlockBaseService::EnableTagOnIOThread,
                     weak_factory_io_thread_.GetWeakPtr(),
                     tag,
                     enabled));
}

void AdBlockBaseService::EnableTagOnIOThread(
    const std::string& tag, bool enabled) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  if (enabled) {
    ad_block_client_->addTag(tag);
  } else {
    ad_block_client_->removeTag(tag);
  }
}

void AdBlockBaseService::GetDATFileData(const base::FilePath& dat_file_path) {
  base::PostTaskAndReplyWithResult(
      GetTaskRunner().get(),
      FROM_HERE,
      base::BindOnce(
          &brave_component_updater::LoadDATFileData<AdBlockClient>,
          dat_file_path),
      base::BindOnce(&AdBlockBaseService::OnGetDATFileData,
                     weak_factory_.GetWeakPtr()));
}

void AdBlockBaseService::OnGetDATFileData(GetDATFileDataResult result) {
  if (result.second.empty()) {
    LOG(ERROR) << "Could not obtain ad block data";
    return;
  }
  if (!result.first.get()) {
    LOG(ERROR) << "Failed to deserialize ad block data";
    return;
  }

  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(&AdBlockBaseService::UpdateAdBlockClient,
                     weak_factory_io_thread_.GetWeakPtr(),
                     std::move(result.first),
                     std::move(result.second)));
}

void AdBlockBaseService::UpdateAdBlockClient(
    std::unique_ptr<AdBlockClient> ad_block_client,
    brave_component_updater::DATFileDataBuffer buffer) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  ad_block_client_ = std::move(ad_block_client);
  buffer_ = std::move(buffer);
}


bool AdBlockBaseService::Init() {
  return true;
}

AdBlockClient* AdBlockBaseService::GetAdBlockClientForTest() {
  return ad_block_client_.get();
}

///////////////////////////////////////////////////////////////////////////////

}  // namespace brave_shields
