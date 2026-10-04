#pragma once

#include <QWidget>

#include <functional>

#include "ui/pages/BasePage.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class Task;

// Settings and maintenance of the shared store, where files shared between instances are kept
class SharedStorePage : public QWidget, public BasePage {
    Q_OBJECT

   public:
    explicit SharedStorePage(QWidget* parent = nullptr);

    QString displayName() const override { return tr("Shared Files"); }
    QIcon icon() const override { return QIcon::fromTheme("viewfolder"); }
    QString id() const override { return "shared-store-settings"; }
    QString helpPage() const override { return "Shared-files"; }
    bool apply() override;
    void retranslate() override {}

   private:
    void loadSettings();
    void refreshStatus();
    // Runs a store task with a progress dialog, then shows what it found
    void runTask(Task* task, const std::function<QString()>& summary);

    QCheckBox* m_enabled = nullptr;
    QComboBox* m_linkMode = nullptr;
    QLineEdit* m_storeDir = nullptr;
    QCheckBox* m_reconcileOnStartup = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_volumeWarning = nullptr;
    QPushButton* m_verify = nullptr;
    QPushButton* m_reconcile = nullptr;
    QPushButton* m_deepVerify = nullptr;
    QPushButton* m_repair = nullptr;
    QPushButton* m_useAnyway = nullptr;
};
