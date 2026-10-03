#pragma once

#include <QString>

#include <cstdint>
#include <optional>

#include "Result.h"
#include "contentstore/StoreFormat.h"
#include "contentstore/StoreLock.h"

// A store of files shared between instances, kept once and linked into each instance that uses them.
//
// Only one launcher process changes a store at a time: the one holding its lock. Other launchers using the same store
// keep working with their existing links, but put new files directly into their instances.
class ContentStore {
   public:
    enum class State : std::uint8_t {
        Closed,
        // this launcher holds the store and may change it
        Writable,
        // this launcher holds the store, but its format needs a newer launcher to change it
        ReadOnly,
        // another launcher holds the store
        Busy,
        // the store can't be used, see statusMessage()
        Disabled,
    };

    ContentStore(QString storeDir, QString dataDir);

    // Opens the store, or tries again if it was busy
    State open();

    // Takes over a store locked by another machine. Only for when the user confirmed that machine no longer uses it.
    State forceOpen();

    State state() const { return m_state; }
    QString statusMessage() const { return m_statusMessage; }
    bool isWritable() const { return m_state == State::Writable; }

    // Identifies this launcher's data folder among the launchers that use the store
    QString clientId() const { return m_clientId; }
    std::optional<StoreLock::Holder> lockHolder() const { return m_lock.holder(); }

    QString storeDir() const { return m_storeDir; }
    QString objectsDir() const;
    QString temporaryDir() const;
    QString retiredDir() const;

    // The id of the launcher using dataDir, created on first use
    static Result<QString> loadClientId(const QString& dataDir);

   private:
    State openWithLock(StoreLock::Status lockStatus);
    State setState(State state, const QString& message = {});

    QString m_storeDir;
    QString m_dataDir;
    QString m_clientId;
    StoreLock m_lock;
    State m_state = State::Closed;
    QString m_statusMessage;
};
