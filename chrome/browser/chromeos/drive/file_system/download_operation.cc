// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/drive/file_system/download_operation.h"

#include "base/file_util.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/task_runner_util.h"
#include "chrome/browser/chromeos/drive/drive.pb.h"
#include "chrome/browser/chromeos/drive/file_cache.h"
#include "chrome/browser/chromeos/drive/file_errors.h"
#include "chrome/browser/chromeos/drive/file_system/operation_observer.h"
#include "chrome/browser/chromeos/drive/file_system_util.h"
#include "chrome/browser/chromeos/drive/job_scheduler.h"
#include "chrome/browser/chromeos/drive/resource_entry_conversion.h"
#include "chrome/browser/chromeos/drive/resource_metadata.h"
#include "chrome/browser/google_apis/gdata_errorcode.h"
#include "content/public/browser/browser_thread.h"

using content::BrowserThread;

namespace drive {
namespace file_system {
namespace {

// If the resource is a hosted document, creates a JSON file representing the
// resource locally, and returns FILE_ERROR_OK with |cache_file_path| storing
// the path to the JSON file.
// If the resource is a regular file and its local cache is available,
// returns FILE_ERROR_OK with |cache_file_path| storing the path to the
// cache file.
// If the resource is a regular file but its local cache is NOT available,
// returns FILE_ERROR_OK, but |cache_file_path| is kept empty.
// Otherwise returns error code.
FileError CheckPreConditionForEnsureFileDownloaded(
    internal::ResourceMetadata* metadata,
    internal::FileCache* cache,
    const base::FilePath& temporary_file_directory,
    ResourceEntry* entry,
    base::FilePath* cache_file_path) {
  DCHECK(metadata);
  DCHECK(cache);
  DCHECK(cache_file_path);

  if (entry->file_info().is_directory())
    return FILE_ERROR_NOT_A_FILE;

  // The file's entry should have its file specific info.
  DCHECK(entry->has_file_specific_info());

  // For a hosted document, we create a special JSON file to represent the
  // document instead of fetching the document content in one of the exported
  // formats. The JSON file contains the edit URL and resource ID of the
  // document.
  if (entry->file_specific_info().is_hosted_document()) {
    base::FilePath gdoc_file_path;
    if (!file_util::CreateTemporaryFileInDir(temporary_file_directory,
                                             &gdoc_file_path) ||
        !util::CreateGDocFile(gdoc_file_path,
                              GURL(entry->file_specific_info().alternate_url()),
                              entry->resource_id()))
      return FILE_ERROR_FAILED;

    *cache_file_path = gdoc_file_path;
    return FILE_ERROR_OK;
  }

  // Get the cache file path if available.
  cache->GetFile(entry->resource_id(),
                 entry->file_specific_info().md5(),
                 cache_file_path);

  // If the cache file is available and dirty, the modified file info needs to
  // be stored in |entry|.
  // TODO(kinaba): crbug.com/246469. The logic below is a duplicate of that in
  // drive::FileSystem::CheckLocalModificationAndRun. We should merge them once
  // the drive::FS side is also converted to run fully on blocking pool.
  if (!cache_file_path->empty()) {
    FileCacheEntry cache_entry;
    if (cache->GetCacheEntry(entry->resource_id(),
                             entry->file_specific_info().md5(),
                             &cache_entry) &&
        cache_entry.is_dirty()) {
      base::PlatformFileInfo file_info;
      if (file_util::GetFileInfo(*cache_file_path, &file_info)) {
        PlatformFileInfoProto entry_file_info;
        util::ConvertPlatformFileInfoToResourceEntry(file_info,
                                                     &entry_file_info);
        *entry->mutable_file_info() = entry_file_info;
      }
    }
  }

  return FILE_ERROR_OK;
}

// Calls CheckPreConditionForEnsureFileDownloaded() with the entry specified by
// the given ID.
FileError CheckPreConditionForEnsureFileDownloadedByResourceId(
    internal::ResourceMetadata* metadata,
    internal::FileCache* cache,
    const std::string& resource_id,
    const base::FilePath& temporary_file_directory,
    base::FilePath* cache_file_path,
    ResourceEntry* entry) {
  FileError error = metadata->GetResourceEntryById(resource_id, entry);
  if (error != FILE_ERROR_OK)
    return error;
  return CheckPreConditionForEnsureFileDownloaded(
      metadata, cache, temporary_file_directory, entry, cache_file_path);
}

// Calls CheckPreConditionForEnsureFileDownloaded() with the entry specified by
// the given file path.
FileError CheckPreConditionForEnsureFileDownloadedByPath(
    internal::ResourceMetadata* metadata,
    internal::FileCache* cache,
    const base::FilePath& file_path,
    const base::FilePath& temporary_file_directory,
    base::FilePath* cache_file_path,
    ResourceEntry* entry) {
  FileError error = metadata->GetResourceEntryByPath(file_path, entry);
  if (error != FILE_ERROR_OK)
    return error;
  return CheckPreConditionForEnsureFileDownloaded(
      metadata, cache, temporary_file_directory, entry, cache_file_path);
}

// Creates a file with unique name in |dir| and stores the path to |temp_file|.
// Additionally, sets the permission of the file to allow read access from
// others and group member users (i.e, "-rw-r--r--").
// We need this wrapper because Drive cache files may be read from other
// processes (e.g., cros_disks for mounting zip files).
bool CreateTemporaryReadableFileInDir(const base::FilePath& dir,
                                      base::FilePath* temp_file) {
  if (!file_util::CreateTemporaryFileInDir(dir, temp_file))
    return false;
  return file_util::SetPosixFilePermissions(
      *temp_file,
      file_util::FILE_PERMISSION_READ_BY_USER |
      file_util::FILE_PERMISSION_WRITE_BY_USER |
      file_util::FILE_PERMISSION_READ_BY_GROUP |
      file_util::FILE_PERMISSION_READ_BY_OTHERS);
}

// Prepares for downloading the file. Given the |gdata_entry|, refreshes the
// |metadata| and then allocates the enough space in the cache.
// If succeeded, returns FILE_ERROR_OK with |entry| storing the ResourceEntry
// of the resource, |drive_file_path| with storing the path of the entry,
// and |temp_download_file| storing the path to the file in the cache.
FileError PrepareForDownloadFile(
    internal::ResourceMetadata* metadata,
    internal::FileCache* cache,
    scoped_ptr<google_apis::ResourceEntry> gdata_entry,
    const base::FilePath& temporary_file_directory,
    ResourceEntry* entry,
    base::FilePath* drive_file_path,
    base::FilePath* temp_download_file) {
  DCHECK(metadata);
  DCHECK(cache);
  DCHECK(gdata_entry);
  DCHECK(entry);
  DCHECK(drive_file_path);
  DCHECK(temp_download_file);

  *entry = ConvertToResourceEntry(*gdata_entry);
  FileError error = metadata->RefreshEntry(*entry);
  if (error != FILE_ERROR_OK)
    return error;

  error = metadata->GetResourceEntryById(entry->resource_id(), entry);
  if (error != FILE_ERROR_OK)
    return error;

  *drive_file_path = metadata->GetFilePath(entry->resource_id());
  if (drive_file_path->empty())
    return FILE_ERROR_NOT_FOUND;

  // Ensure enough space in the cache.
  if (!cache->FreeDiskSpaceIfNeededFor(entry->file_info().size()))
    return FILE_ERROR_NO_SPACE;

  // Create the temporary file which will store the donwloaded content.
  return CreateTemporaryReadableFileInDir(
      temporary_file_directory,
      temp_download_file) ? FILE_ERROR_OK : FILE_ERROR_FAILED;
}

// Stores the downloaded file at |downloaded_file_path| into |cache|.
// If succeeded, returns FILE_ERROR_OK with |cache_file_path| storing the
// path to the cache file.
// If failed, returns an error code with deleting |downloaded_file_path|.
FileError UpdateLocalStateForDownloadFile(
    internal::FileCache* cache,
    const std::string& resource_id,
    const std::string& md5,
    google_apis::GDataErrorCode gdata_error,
    const base::FilePath& downloaded_file_path,
    base::FilePath* cache_file_path) {
  DCHECK(cache);

  FileError error = util::GDataToFileError(gdata_error);
  if (error != FILE_ERROR_OK) {
    file_util::Delete(downloaded_file_path, false /* recursive */);
    return error;
  }

  // Here the download is completed successfully, so store it into the cache.
  error = cache->Store(resource_id, md5, downloaded_file_path,
                       internal::FileCache::FILE_OPERATION_MOVE);
  if (error != FILE_ERROR_OK) {
    file_util::Delete(downloaded_file_path, false /* recursive */);
    return error;
  }

  return cache->GetFile(resource_id, md5, cache_file_path);
}

}  // namespace

class DownloadOperation::DownloadCallback {
 public:
  DownloadCallback(
      const GetFileContentInitializedCallback initialized_callback,
      const google_apis::GetContentCallback get_content_callback,
      const GetFileCallback completion_callback)
      : initialized_callback_(initialized_callback),
        get_content_callback_(get_content_callback),
        completion_callback_(completion_callback) {
    DCHECK(!completion_callback_.is_null());
  }

  void OnCacheFileFound(const ResourceEntry& entry,
                        const base::FilePath& cache_file_path) const {
    if (initialized_callback_.is_null())
      return;

    initialized_callback_.Run(
        FILE_ERROR_OK, make_scoped_ptr(new ResourceEntry(entry)),
        cache_file_path, base::Closure());
  }

  void OnStartDownloading(const ResourceEntry& entry,
                          const base::Closure& cancel_download_closure) const {
    if (initialized_callback_.is_null()) {
      return;
    }

    initialized_callback_.Run(
        FILE_ERROR_OK, make_scoped_ptr(new ResourceEntry(entry)),
        base::FilePath(), cancel_download_closure);
  }

  void OnError(FileError error) const {
    completion_callback_.Run(
        error, base::FilePath(), scoped_ptr<ResourceEntry>());
  }

  void OnComplete(const base::FilePath& cache_file_path,
                  scoped_ptr<ResourceEntry> entry) const {
    completion_callback_.Run(FILE_ERROR_OK, cache_file_path, entry.Pass());
  }

  const google_apis::GetContentCallback& get_content_callback() const {
    return get_content_callback_;
  }

 private:
  const GetFileContentInitializedCallback initialized_callback_;
  const google_apis::GetContentCallback get_content_callback_;
  const GetFileCallback completion_callback_;

  // This class is copiable.
};

struct DownloadOperation::DownloadParams {
  DownloadParams(const ClientContext& context,
                 const GURL& download_url)
      : context(context),
        download_url(download_url),
        entry(new ResourceEntry) {
  }

  ClientContext context;
  GURL download_url;
  scoped_ptr<ResourceEntry> entry;
  base::FilePath drive_file_path;
  base::FilePath temp_download_file_path;
};

DownloadOperation::DownloadOperation(
    base::SequencedTaskRunner* blocking_task_runner,
    OperationObserver* observer,
    JobScheduler* scheduler,
    internal::ResourceMetadata* metadata,
    internal::FileCache* cache,
    const base::FilePath& temporary_file_directory)
    : blocking_task_runner_(blocking_task_runner),
      observer_(observer),
      scheduler_(scheduler),
      metadata_(metadata),
      cache_(cache),
      temporary_file_directory_(temporary_file_directory),
      weak_ptr_factory_(this) {
}

DownloadOperation::~DownloadOperation() {
}

void DownloadOperation::EnsureFileDownloadedByResourceId(
    const std::string& resource_id,
    const ClientContext& context,
    const GetFileContentInitializedCallback& initialized_callback,
    const google_apis::GetContentCallback& get_content_callback,
    const GetFileCallback& completion_callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!completion_callback.is_null());

  DownloadCallback callback(
      initialized_callback, get_content_callback, completion_callback);

  ResourceEntry* entry = new ResourceEntry;
  base::FilePath* cache_file_path = new base::FilePath;
  base::PostTaskAndReplyWithResult(
      blocking_task_runner_.get(),
      FROM_HERE,
      base::Bind(&CheckPreConditionForEnsureFileDownloadedByResourceId,
                 base::Unretained(metadata_),
                 base::Unretained(cache_),
                 resource_id,
                 temporary_file_directory_,
                 cache_file_path,
                 entry),
      base::Bind(&DownloadOperation::EnsureFileDownloadedAfterCheckPreCondition,
                 weak_ptr_factory_.GetWeakPtr(),
                 context,
                 callback,
                 base::Passed(make_scoped_ptr(entry)),
                 base::Owned(cache_file_path)));
}

void DownloadOperation::EnsureFileDownloadedByPath(
    const base::FilePath& file_path,
    const ClientContext& context,
    const GetFileContentInitializedCallback& initialized_callback,
    const google_apis::GetContentCallback& get_content_callback,
    const GetFileCallback& completion_callback) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(!completion_callback.is_null());

  DownloadCallback callback(
      initialized_callback, get_content_callback, completion_callback);

  ResourceEntry* entry = new ResourceEntry;
  base::FilePath* cache_file_path = new base::FilePath;
  base::PostTaskAndReplyWithResult(
      blocking_task_runner_.get(),
      FROM_HERE,
      base::Bind(&CheckPreConditionForEnsureFileDownloadedByPath,
                 base::Unretained(metadata_),
                 base::Unretained(cache_),
                 file_path,
                 temporary_file_directory_,
                 cache_file_path,
                 entry),
      base::Bind(&DownloadOperation::EnsureFileDownloadedAfterCheckPreCondition,
                 weak_ptr_factory_.GetWeakPtr(),
                 context,
                 callback,
                 base::Passed(make_scoped_ptr(entry)),
                 base::Owned(cache_file_path)));
}

void DownloadOperation::EnsureFileDownloadedAfterCheckPreCondition(
    const ClientContext& context,
    const DownloadCallback& callback,
    scoped_ptr<ResourceEntry> entry,
    base::FilePath* cache_file_path,
    FileError error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(entry);
  DCHECK(cache_file_path);

  if (error != FILE_ERROR_OK) {
    // During precondition check, an error is found.
    callback.OnError(error);
    return;
  }

  if (!cache_file_path->empty()) {
    // The cache file is found.
    callback.OnCacheFileFound(*entry, *cache_file_path);
    callback.OnComplete(*cache_file_path, entry.Pass());
    return;
  }

  // If cache file is not found, try to download the file from the server
  // instead. This logic is rather complicated but here's how this works:
  //
  // Retrieve fresh file metadata from server. We will extract file size and
  // download url from there. Note that the download url is transient.
  //
  // Check if we have enough space, based on the expected file size.
  // - if we don't have enough space, try to free up the disk space
  // - if we still don't have enough space, return "no space" error
  // - if we have enough space, start downloading the file from the server
  scheduler_->GetResourceEntry(
      entry->resource_id(),
      context,
      base::Bind(&DownloadOperation::EnsureFileDownloadedAfterGetResourceEntry,
                 weak_ptr_factory_.GetWeakPtr(),
                 context,
                 callback));
}

void DownloadOperation::EnsureFileDownloadedAfterGetResourceEntry(
    const ClientContext& context,
    const DownloadCallback& callback,
    google_apis::GDataErrorCode gdata_error,
    scoped_ptr<google_apis::ResourceEntry> resource_entry) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  FileError error = util::GDataToFileError(gdata_error);
  if (error != FILE_ERROR_OK) {
    callback.OnError(error);
    return;
  }
  DCHECK(resource_entry);

  // The download URL is:
  // 1) src attribute of content element, on GData WAPI.
  // 2) the value of the key 'downloadUrl', on Drive API v2.
  // In both cases, we can use ResourceEntry::download_url().
  const GURL& download_url = resource_entry->download_url();

  // The download URL can be empty for non-downloadable files (such as files
  // shared from others with "prevent downloading by viewers" flag set.)
  if (download_url.is_empty()) {
    callback.OnError(FILE_ERROR_ACCESS_DENIED);
    return;
  }

  // Before starting to download actually, refresh the metadata and allocate
  // the cache space.
  DownloadParams* params = new DownloadParams(context, download_url);
  base::PostTaskAndReplyWithResult(
      blocking_task_runner_.get(),
      FROM_HERE,
      base::Bind(&PrepareForDownloadFile,
                 base::Unretained(metadata_),
                 base::Unretained(cache_),
                 base::Passed(&resource_entry),
                 temporary_file_directory_,
                 params->entry.get(),
                 &params->drive_file_path,
                 &params->temp_download_file_path),
      base::Bind(
          &DownloadOperation::EnsureFileDownloadedAfterPrepareForDownloadFile,
          weak_ptr_factory_.GetWeakPtr(),
          base::Owned(params),
          callback));
}

void DownloadOperation::EnsureFileDownloadedAfterPrepareForDownloadFile(
    DownloadParams* params,
    const DownloadCallback& callback,
    FileError error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));
  DCHECK(params);

  if (error != FILE_ERROR_OK) {
    callback.OnError(error);
    return;
  }

  ResourceEntry* entry_ptr = params->entry.get();
  JobID id = scheduler_->DownloadFile(
      params->drive_file_path,
      params->temp_download_file_path,
      params->download_url,
      params->context,
      base::Bind(&DownloadOperation::EnsureFileDownloadedAfterDownloadFile,
                 weak_ptr_factory_.GetWeakPtr(),
                 params->drive_file_path,
                 base::Passed(&params->entry),
                 callback),
      callback.get_content_callback());

  // Notify via |initialized_callback| if necessary.
  callback.OnStartDownloading(
      *entry_ptr,
      base::Bind(&DownloadOperation::CancelJob,
                 weak_ptr_factory_.GetWeakPtr(), id));
}

void DownloadOperation::EnsureFileDownloadedAfterDownloadFile(
    const base::FilePath& drive_file_path,
    scoped_ptr<ResourceEntry> entry,
    const DownloadCallback& callback,
    google_apis::GDataErrorCode gdata_error,
    const base::FilePath& downloaded_file_path) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  ResourceEntry* entry_ptr = entry.get();
  base::FilePath* cache_file_path = new base::FilePath;
  base::PostTaskAndReplyWithResult(
      blocking_task_runner_.get(),
      FROM_HERE,
      base::Bind(&UpdateLocalStateForDownloadFile,
                 base::Unretained(cache_),
                 entry_ptr->resource_id(),
                 entry_ptr->file_specific_info().md5(),
                 gdata_error,
                 downloaded_file_path,
                 cache_file_path),
      base::Bind(&DownloadOperation::EnsureFileDownloadedAfterUpdateLocalState,
                 weak_ptr_factory_.GetWeakPtr(),
                 drive_file_path,
                 callback,
                 base::Passed(&entry),
                 base::Owned(cache_file_path)));
}

void DownloadOperation::EnsureFileDownloadedAfterUpdateLocalState(
    const base::FilePath& file_path,
    const DownloadCallback& callback,
    scoped_ptr<ResourceEntry> entry,
    base::FilePath* cache_file_path,
    FileError error) {
  DCHECK(BrowserThread::CurrentlyOn(BrowserThread::UI));

  if (error != FILE_ERROR_OK) {
    callback.OnError(error);
    return;
  }

  // Storing to cache changes the "offline available" status, hence notify.
  observer_->OnDirectoryChangedByOperation(file_path.DirName());
  callback.OnComplete(*cache_file_path, entry.Pass());
}

void DownloadOperation::CancelJob(JobID job_id) {
  scheduler_->CancelJob(job_id);
}

}  // namespace file_system
}  // namespace drive
