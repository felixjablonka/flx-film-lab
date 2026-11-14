#pragma once

#include <QString>

class Document;
class MotionDocument;
class MainWindow;

// The folder workflow driven as the sidebar drives it, in the running app
// (tests/app folders(), tests/motionapp clipFolders() and gradedThumbnails()
// on the Mac): `pictures` is a folder with RAW files, of which four are copied
// into a scratch folder; `clips` a folder of log clips, read where they are.
// The library and the sidebar's prints are kept in `scratch`. Prints each
// check; returns the number that failed.
int runSelfTest(MainWindow &window, Document &document, MotionDocument &motion, const QString &pictures,
                const QString &clips, const QString &scratch);
