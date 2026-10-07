#include <QApplication>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <atomic>

#include "contentstore/SharingActionTask.h"
#include "ui/dialogs/ProgressDialog.h"

// A progress dialog stays until its task finished, so nothing the task does afterwards is lost
class ProgressDialogTest : public QObject {
    Q_OBJECT

   private slots:
    void test_escapeWaitsForTheTask()
    {
        std::atomic<bool> released = false;
        bool followedUp = false;
        // a task that is still running when Escape is pressed, and only then is let go
        SharingActionTask task("Testing", { [&released, &followedUp]() -> Result<SharingActionTask::FollowUp> {
                                   while (!released) {
                                       QThread::msleep(10);
                                   }
                                   return [&followedUp] { followedUp = true; };
                               } });
        ProgressDialog dialog;
        QTimer::singleShot(200, &dialog, [&dialog] { QTest::keyClick(&dialog, Qt::Key_Escape); });
        QTimer::singleShot(400, &dialog, [&released, &dialog] {
            // the dialog is still there
            QVERIFY(dialog.isVisible());
            released = true;
        });
        dialog.execWithTask(&task);
        // let the job go in any case, so a failure here doesn't leave the task waiting for it
        const bool releasedBeforeReturning = released;
        released = true;
        QVERIFY(releasedBeforeReturning);
        QVERIFY(task.isFinished());
        QVERIFY(followedUp);
    }
};

int main(int argc, char* argv[])
{
    // no display is needed
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QApplication app(argc, argv);
    ProgressDialogTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ProgressDialog_test.moc"
