// Author: Ovidiu Predescu
//
// Test the file change notification mechanism.

#include <whisperlib/io/file/file.h>
#include <whisperlib/net/selector.h>
#include <whisperlib/base/log.h>
#include <whisperlib/base/system.h>

int file_changed_notification = 0;
bool file_renamed = false;

void UpdateFile(net::Selector* selector, io::File* file) {
  LOG_INFO << "Updating file";
  file->Write("bla\n");
  file->Write("foo\n");
}

void ExpandFile(net::Selector* selector, io::File* file) {
  LOG_INFO << "Expanding file";
  file->Truncate(100);
}

void RenameFile(net::Selector* selector, io::File* file) {
  LOG_INFO << "Renaming file";
  file->Rename("/tmp/whisperlib-file-change-test-file2");
}

void FileWasChanged(net::Selector* selector,
                    io::File* file,
                    int event) {
  LOG_INFO << "File " << file->filename()
           << " was modified: 0x" << std::hex << event;
  if (event & net::Selector::kFileModified) {
    file_changed_notification++;
  }
  if (event & net::Selector::kFileRenamed) {
    LOG_INFO << "File was renamed to " << file->filename();
    file_renamed = true;
    selector->MakeLoopExit();
  }

  switch (file_changed_notification) {
    case 1:
      selector->RunInSelectLoop(
          NewPermanentCallback(ExpandFile, selector, file));
      break;
    case 2:
      selector->RunInSelectLoop(
          NewPermanentCallback(RenameFile, selector, file));
      break;
  }
}

int main(int argc, char** argv) {
  FLAGS_logtostderr = 1;
  FLAGS_v = 0;
  common::Init(argc, argv);

  net::Selector selector;
  io::File file;
  const char* fname = "/tmp/whisperlib-file-change-test-file";

  CHECK(file.Open(fname, io::File::GENERIC_WRITE, io::File::CREATE_ALWAYS));

  LOG_INFO << "File descriptor " << file.fd();

  // Registering the file for change notification must succeed.
  CHECK(selector.RegisterFileForChangeNotifications(
      &file, NewPermanentCallback(FileWasChanged)));

  // The second time we call the registration must not succeed, as
  // we've already registered.
  CHECK(!selector.RegisterFileForChangeNotifications(
      &file, NewPermanentCallback(FileWasChanged)));

  // Unregistering must succeed.
  CHECK(selector.UnregisterFileForChangeNotifications(&file));

  // Unregistering when not register must not succeed.
  CHECK(!selector.UnregisterFileForChangeNotifications(&file));

  // Now register the file for changes and lets do some work.
  CHECK(selector.RegisterFileForChangeNotifications(
      &file, NewPermanentCallback(FileWasChanged)));

  selector.RunInSelectLoop(NewPermanentCallback(UpdateFile, &selector, &file));
  selector.Loop();
  LOG_INFO << "Done";
  CHECK_EQ(file_changed_notification, 2);
  CHECK(file_renamed);
}
