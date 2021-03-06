/*
 * Copyright (C) 2013 Jolla Mobile <andrew.den.exter@jollamobile.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include "seasidecache.h"

#include "seasideperson.h"
#include "normalization_p.h"
#include "synchronizelists_p.h"
#include "constants_p.h"

#include <QCoreApplication>
#ifdef USING_QTPIM
#include <QStandardPaths>
#else
#include <QDesktopServices>
#endif
#include <QDir>
#include <QEvent>
#include <QFile>

#include <QContactAvatar>
#include <QContactDetailFilter>
#include <QContactEmailAddress>
#include <QContactFavorite>
#include <QContactName>
#include <QContactOnlineAccount>
#include <QContactOrganization>
#include <QContactPhoneNumber>
#include <QContactGlobalPresence>
#include <QContactSyncTarget>

#include <QVersitContactExporter>
#include <QVersitContactImporter>
#include <QVersitReader>
#include <QVersitWriter>

#include <QtDebug>

USE_VERSIT_NAMESPACE

static QList<QChar> getAllContactNameGroups()
{
    QList<QChar> groups;
    groups << QLatin1Char('A')
           << QLatin1Char('B')
           << QLatin1Char('C')
           << QLatin1Char('D')
           << QLatin1Char('E')
           << QLatin1Char('F')
           << QLatin1Char('G')
           << QLatin1Char('H')
           << QLatin1Char('I')
           << QLatin1Char('J')
           << QLatin1Char('K')
           << QLatin1Char('L')
           << QLatin1Char('M')
           << QLatin1Char('N')
           << QLatin1Char('O')
           << QLatin1Char('P')
           << QLatin1Char('Q')
           << QLatin1Char('R')
           << QLatin1Char('S')
           << QLatin1Char('T')
           << QLatin1Char('U')
           << QLatin1Char('V')
           << QLatin1Char('W')
           << QLatin1Char('X')
           << QLatin1Char('Y')
           << QLatin1Char('Z')
           << QChar(0x00c5)     // Å
           << QChar(0x00c4)     // Ä
           << QChar(0x00d6)     // Ö
           << QLatin1Char('#');
    return groups;
}

SeasideCache *SeasideCache::instance = 0;
QList<QChar> SeasideCache::allContactNameGroups = getAllContactNameGroups();

static QString managerName()
{
#ifdef USING_QTPIM
    // Temporary override until qtpim supports QTCONTACTS_MANAGER_OVERRIDE
    return QStringLiteral("org.nemomobile.contacts.sqlite");
#endif
    QByteArray environmentManager = qgetenv("NEMO_CONTACT_MANAGER");
    return !environmentManager.isEmpty()
            ? QString::fromLatin1(environmentManager, environmentManager.length())
            : QString();
}

template<typename T, typename Filter, typename Field>
void setDetailType(Filter &filter, Field field)
{
#ifdef USING_QTPIM
    filter.setDetailType(T::Type, field);
#else
    filter.setDetailDefinitionName(T::DefinitionName, field);
#endif
}

SeasideCache::SeasideCache()
    : m_manager(managerName())
#ifdef HAS_MLITE
    , m_displayLabelOrderConf(QLatin1String("/org/nemomobile/contacts/display_label_order"))
#endif
    , m_resultsRead(0)
    , m_populated(0)
    , m_cacheIndex(0)
    , m_queryIndex(0)
    , m_appendIndex(0)
    , m_fetchFilter(SeasideFilteredModel::FilterFavorites)
    , m_displayLabelOrder(SeasideFilteredModel::FirstNameFirst)
    , m_updatesPending(true)
    , m_refreshRequired(false)
    , m_contactsUpdated(false)
{
    Q_ASSERT(!instance);
    instance = this;

    m_timer.start();

#ifdef HAS_MLITE
    connect(&m_displayLabelOrderConf, SIGNAL(valueChanged()), this, SLOT(displayLabelOrderChanged()));
    QVariant displayLabelOrder = m_displayLabelOrderConf.value();
    if (displayLabelOrder.isValid())
        m_displayLabelOrder = SeasideFilteredModel::DisplayLabelOrder(displayLabelOrder.toInt());
#endif

#ifdef USING_QTPIM
    connect(&m_manager, SIGNAL(dataChanged()), this, SLOT(updateContacts()));
    connect(&m_manager, SIGNAL(contactsChanged(QList<QContactId>)),
            this, SLOT(updateContacts(QList<QContactId>)));
    connect(&m_manager, SIGNAL(contactsAdded(QList<QContactId>)),
            this, SLOT(updateContacts(QList<QContactId>)));
    connect(&m_manager, SIGNAL(contactsRemoved(QList<QContactId>)),
            this, SLOT(contactsRemoved(QList<QContactId>)));
#else
    connect(&m_manager, SIGNAL(dataChanged()), this, SLOT(updateContacts()));
    connect(&m_manager, SIGNAL(contactsChanged(QList<QContactLocalId>)),
            this, SLOT(updateContacts(QList<QContactLocalId>)));
    connect(&m_manager, SIGNAL(contactsAdded(QList<QContactLocalId>)),
            this, SLOT(updateContacts(QList<QContactLocalId>)));
    connect(&m_manager, SIGNAL(contactsRemoved(QList<QContactLocalId>)),
            this, SLOT(contactsRemoved(QList<QContactLocalId>)));
#endif

    connect(&m_fetchRequest, SIGNAL(resultsAvailable()), this, SLOT(contactsAvailable()));
    connect(&m_fetchByIdRequest, SIGNAL(resultsAvailable()), this, SLOT(contactsAvailable()));
    connect(&m_contactIdRequest, SIGNAL(resultsAvailable()), this, SLOT(contactIdsAvailable()));
    connect(&m_relationshipsFetchRequest, SIGNAL(resultsAvailable()), this, SLOT(relationshipsAvailable()));

    connect(&m_fetchRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_fetchByIdRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_contactIdRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_relationshipsFetchRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_removeRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));
    connect(&m_saveRequest, SIGNAL(stateChanged(QContactAbstractRequest::State)),
            this, SLOT(requestStateChanged(QContactAbstractRequest::State)));

    m_fetchRequest.setManager(&m_manager);
    m_fetchByIdRequest.setManager(&m_manager);
    m_contactIdRequest.setManager(&m_manager);
    m_relationshipsFetchRequest.setManager(&m_manager);
    m_removeRequest.setManager(&m_manager);
    m_saveRequest.setManager(&m_manager);

    QContactFetchHint fetchHint;
    fetchHint.setOptimizationHints(QContactFetchHint::NoRelationships
            | QContactFetchHint::NoActionPreferences
            | QContactFetchHint::NoBinaryBlobs);

    /* Possibly too aggressive - this filters out details we may need, such as nickname.
       Remove until performance testing indicates it is worthwhile.
    fetchHint.setDetailDefinitionsHint(QStringList()
            << QContactName::DefinitionName
            << QContactAvatar::DefinitionName
            << QContactPhoneNumber::DefinitionName
            << QContactEmailAddress::DefinitionName
            << QContactOrganization::DefinitionName
            << QContactOnlineAccount::DefinitionName);
    */

    m_fetchRequest.setFetchHint(fetchHint);
    m_fetchRequest.setFilter(QContactFavorite::match());

    QContactSortOrder firstLabelOrder;
    setDetailType<QContactName>(firstLabelOrder, QContactName::FieldFirstName);
    firstLabelOrder.setCaseSensitivity(Qt::CaseInsensitive);
    firstLabelOrder.setDirection(Qt::AscendingOrder);
    firstLabelOrder.setBlankPolicy(QContactSortOrder::BlanksFirst);

    QContactSortOrder secondLabelOrder;
    setDetailType<QContactName>(secondLabelOrder, QContactName::FieldLastName);
    secondLabelOrder.setCaseSensitivity(Qt::CaseInsensitive);
    secondLabelOrder.setDirection(Qt::AscendingOrder);
    secondLabelOrder.setBlankPolicy(QContactSortOrder::BlanksFirst);

    QList<QContactSortOrder> sorting = m_displayLabelOrder == SeasideFilteredModel::FirstNameFirst
            ? (QList<QContactSortOrder>() << firstLabelOrder << secondLabelOrder)
            : (QList<QContactSortOrder>() << secondLabelOrder << firstLabelOrder);

    m_fetchRequest.setSorting(sorting);
    m_contactIdRequest.setSorting(sorting);

    m_fetchRequest.start();
}

SeasideCache::~SeasideCache()
{
    if (instance == this)
        instance = 0;
}

void SeasideCache::checkForExpiry()
{
    if (instance->m_users.isEmpty()) {
        bool unused = true;
        for (int i = 0; i < SeasideFilteredModel::FilterTypesCount; ++i) {
            unused &= instance->m_models[i].isEmpty();
        }
        if (unused) {
            instance->m_expiryTimer.start(30000, instance);
        }
    }
}

void SeasideCache::registerModel(SeasideFilteredModel *model, SeasideFilteredModel::FilterType type)
{
    if (!instance) {
        new SeasideCache;
    } else {
        instance->m_expiryTimer.stop();
        for (int i = 0; i < SeasideFilteredModel::FilterTypesCount; ++i)
            instance->m_models[i].removeAll(model);
    }
    instance->m_models[type].append(model);
}

void SeasideCache::unregisterModel(SeasideFilteredModel *model)
{
    for (int i = 0; i < SeasideFilteredModel::FilterTypesCount; ++i)
        instance->m_models[i].removeAll(model);

    checkForExpiry();
}

void SeasideCache::registerUser(QObject *user)
{
    if (!instance) {
        new SeasideCache;
    } else {
        instance->m_expiryTimer.stop();
    }
    instance->m_users.insert(user);
}

void SeasideCache::unregisterUser(QObject *user)
{
    instance->m_users.remove(user);

    checkForExpiry();
}

void SeasideCache::registerNameGroupChangeListener(SeasideNameGroupChangeListener *listener)
{
    if (!instance)
        new SeasideCache;
    instance->m_nameGroupChangeListeners.append(listener);
}

void SeasideCache::unregisterNameGroupChangeListener(SeasideNameGroupChangeListener *listener)
{
    if (!instance)
        return;
    instance->m_nameGroupChangeListeners.removeAll(listener);
}

QChar SeasideCache::nameGroupForCacheItem(SeasideCacheItem *cacheItem)
{
    if (!cacheItem)
        return QChar();

    QChar group;
    QString first;
    QString last;
    QContactName nameDetail = cacheItem->contact.detail<QContactName>();
    if (SeasideCache::displayLabelOrder() == SeasideFilteredModel::FirstNameFirst) {
        first = nameDetail.firstName();
        last = nameDetail.lastName();
    } else {
        first = nameDetail.lastName();
        last = nameDetail.firstName();
    }
    if (!first.isEmpty()) {
        group = first[0].toUpper();
    } else if (!last.isEmpty()) {
        group = last[0].toUpper();
    } else {
        QString displayLabel = (cacheItem->person)
                ? cacheItem->person->displayLabel()
                : SeasidePerson::generateDisplayLabel(cacheItem->contact);
        if (!displayLabel.isEmpty())
            group = displayLabel[0].toUpper();
    }

    // XXX temporary workaround for non-latin names: use non-name details to try to find a
    // latin character group
    if (!group.isNull() && group.toLatin1() != group) {
        QString displayLabel = SeasidePerson::generateDisplayLabelFromNonNameDetails(cacheItem->contact);
        if (!displayLabel.isEmpty())
            group = displayLabel[0].toUpper();
    }

    if (group.isNull() || !allContactNameGroups.contains(group)) {
        group = QLatin1Char('#');   // 'other' group
    }
    return group;
}

QList<QChar> SeasideCache::allNameGroups()
{
    return allContactNameGroups;
}

QHash<QChar, int> SeasideCache::nameGroupCounts()
{
    if (instance)
        return instance->m_contactNameGroups;
    return QHash<QChar, int>();
}

SeasideFilteredModel::DisplayLabelOrder SeasideCache::displayLabelOrder()
{
    return instance->m_displayLabelOrder;
}

int SeasideCache::contactId(const QContact &contact)
{
    quint32 internal = SeasideFilteredModel::internalId(contact);
    return static_cast<int>(internal);
}

SeasidePerson *SeasideCache::personById(const ContactIdType &id)
{
    if (!SeasideFilteredModel::validId(id))
        return 0;

    quint32 iid = SeasideFilteredModel::internalId(id);

    QHash<quint32, SeasideCacheItem>::iterator it = instance->m_people.find(iid);
    if (it != instance->m_people.end()) {
        return person(&(*it));
    } else {
        // Insert a new item into the cache if the one doesn't exist.
        SeasideCacheItem &cacheItem = instance->m_people[iid];
#ifdef USING_QTPIM
        cacheItem.contact.setId(id);
#else
        QContactId contactId;
        contactId.setLocalId(id);
        cacheItem.contact.setId(contactId);
#endif
        return person(&cacheItem);
    }
}

#ifdef USING_QTPIM
SeasidePerson *SeasideCache::personById(int id)
{
    if (id != 0) {
        QContactId contactId(SeasideFilteredModel::apiId(static_cast<quint32>(id)));
        if (!contactId.isNull()) {
            return personById(contactId);
        }
    }

    return 0;
}
#endif

SeasideCacheItem *SeasideCache::cacheItemById(const ContactIdType &id)
{
    quint32 iid = SeasideFilteredModel::internalId(id);

    QHash<quint32, SeasideCacheItem>::iterator it = instance->m_people.find(iid);
    return it != instance->m_people.end()
            ? &(*it)
            : 0;
}

QContact SeasideCache::contactById(const ContactIdType &id)
{
    quint32 iid = SeasideFilteredModel::internalId(id);
    return instance->m_people.value(iid, SeasideCacheItem()).contact;
}

SeasidePerson *SeasideCache::personByPhoneNumber(const QString &msisdn)
{
    QString normalizedNumber = Normalization::normalizePhoneNumber(msisdn);
    QHash<QString, quint32>::const_iterator it = instance->m_phoneNumberIds.find(normalizedNumber);
    if (it != instance->m_phoneNumberIds.end())
        return personById(*it);
    return 0;
}

SeasidePerson *SeasideCache::selfPerson()
{
    return personById(instance->m_manager.selfContactId());
}

SeasidePerson *SeasideCache::person(SeasideCacheItem *cacheItem)
{
    if (!cacheItem->person) {
        cacheItem->person = new SeasidePerson(instance);
        cacheItem->person->setContact(cacheItem->contact);
        cacheItem->filterKey.clear();

        if (!cacheItem->hasCompleteContact) {
            // the name is a little incomplete, it's has complete or has requested complete contact.
            cacheItem->person->setComplete(false);
            cacheItem->hasCompleteContact = true;
            instance->m_changedContacts.append(cacheItem->apiId());
            instance->fetchContacts();
        }
    }
    return cacheItem->person;
}

void SeasideCache::requestUpdate()
{
    if (!m_updatesPending)
        QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
    m_updatesPending = true;
}

bool SeasideCache::savePerson(SeasidePerson *person)
{
    QContact contact = person->contact();

    ContactIdType id = SeasideFilteredModel::apiId(contact);
    if (SeasideFilteredModel::validId(id)) {
        instance->m_contactsToSave[id] = contact;

        instance->updateContactData(id, SeasideFilteredModel::FilterFavorites);
        instance->updateContactData(id, SeasideFilteredModel::FilterOnline);
        instance->updateContactData(id, SeasideFilteredModel::FilterAll);
    } else {
        instance->m_contactsToCreate.append(contact);
    }

    instance->requestUpdate();

    return true;
}

void SeasideCache::updateContactData(
        const ContactIdType &contactId, SeasideFilteredModel::FilterType filter)
{
    int row = m_contacts[filter].indexOf(contactId);

    QList<SeasideFilteredModel *> &models = m_models[filter];
    for (int i = 0; row != -1 && i < models.count(); ++i)
        models.at(i)->sourceDataChanged(row, row);
}

void SeasideCache::removePerson(SeasidePerson *person)
{
    ContactIdType id = SeasideFilteredModel::apiId(person->contact());

    instance->m_contactsToRemove.append(id);
    instance->removeContactData(id, SeasideFilteredModel::FilterFavorites);
    instance->removeContactData(id, SeasideFilteredModel::FilterOnline);
    instance->removeContactData(id, SeasideFilteredModel::FilterAll);

    instance->requestUpdate();
}

void SeasideCache::removeContactData(
        const ContactIdType &contactId, SeasideFilteredModel::FilterType filter)
{
    int row = m_contacts[filter].indexOf(contactId);
    if (row == -1)
        return;

    QList<SeasideFilteredModel *> &models = m_models[filter];
    for (int i = 0; i < models.count(); ++i)
        models.at(i)->sourceAboutToRemoveItems(row, row);

    m_contacts[filter].remove(row);

    for (int i = 0; i < models.count(); ++i)
        models.at(i)->sourceItemsRemoved();
}

void SeasideCache::fetchConstituents(SeasidePerson *person)
{
    QContactId personId(person->contact().id());

    if (!instance->m_contactsToFetchConstituents.contains(personId)) {
        instance->m_contactsToFetchConstituents.append(personId);
        instance->requestUpdate();
    }
}

const QVector<SeasideCache::ContactIdType> *SeasideCache::contacts(SeasideFilteredModel::FilterType type)
{
    return &instance->m_contacts[type];
}

bool SeasideCache::isPopulated(SeasideFilteredModel::FilterType filterType)
{
    return instance->m_populated & (1 << filterType);
}

bool SeasideCache::event(QEvent *event)
{
    if (event->type() != QEvent::UpdateRequest) {
        return QObject::event(event);
    } else if (!m_contactsToRemove.isEmpty()) {
        m_removeRequest.setContactIds(m_contactsToRemove);
        m_removeRequest.start();

        m_contactsToRemove.clear();
    } else if (!m_contactsToCreate.isEmpty() || !m_contactsToSave.isEmpty()) {
        m_contactsToCreate.reserve(m_contactsToCreate.count() + m_contactsToSave.count());

        typedef QHash<ContactIdType, QContact>::iterator iterator;
        for (iterator it = m_contactsToSave.begin(); it != m_contactsToSave.end(); ++it) {
            m_contactsToCreate.append(*it);
        }

        m_saveRequest.setContacts(m_contactsToCreate);
        m_saveRequest.start();

        m_contactsToCreate.clear();
        m_contactsToSave.clear();
    } else if (!m_constituentIds.isEmpty()) {
        // Fetch the constituent information (even if they're already in the
        // cache, because we don't update non-aggregates on change notifications)
#ifdef USING_QTPIM
        m_fetchByIdRequest.setIds(m_constituentIds);
#else
        m_fetchByIdRequest.setLocalIds(m_constituentIds);
#endif
        m_fetchByIdRequest.start();
    } else if (!m_contactsToFetchConstituents.isEmpty()) {
        QContactId aggregateId = m_contactsToFetchConstituents.first();

        // Find the constituents of this contact
#ifdef USING_QTPIM
        QContact first;
        first.setId(aggregateId);
        m_relationshipsFetchRequest.setFirst(first);
        m_relationshipsFetchRequest.setRelationshipType(QContactRelationship::Aggregates());
#else
        m_relationshipsFetchRequest.setFirst(aggregateId);
        m_relationshipsFetchRequest.setRelationshipType(QContactRelationship::Aggregates);
#endif

        m_relationshipsFetchRequest.start();
    } else if (!m_changedContacts.isEmpty()) {
        m_resultsRead = 0;

#ifdef USING_QTPIM
        QContactIdFilter filter;
#else
        QContactLocalIdFilter filter;
#endif
        filter.setIds(m_changedContacts);
        m_changedContacts.clear();

        // A local ID filter will fetch all contacts, rather than just aggregates;
        // we only want to retrieve aggregate contacts that have changed
        QContactDetailFilter stFilter;
        setDetailType<QContactSyncTarget>(stFilter, QContactSyncTarget::FieldSyncTarget);
        stFilter.setValue("aggregate");

        m_appendIndex = 0;
        m_fetchRequest.setFilter(filter & stFilter);
        m_fetchRequest.start();
    } else if (m_refreshRequired) {
        m_resultsRead = 0;
        m_refreshRequired = false;
        m_fetchFilter = SeasideFilteredModel::FilterFavorites;

        m_contactIdRequest.setFilter(QContactFavorite::match());
        m_contactIdRequest.start();
    } else {
        m_updatesPending = false;

        const QHash<ContactIdType,int> expiredContacts = m_expiredContacts;
        m_expiredContacts.clear();

        typedef QHash<ContactIdType,int>::const_iterator iterator;
        for (iterator it = expiredContacts.begin(); it != expiredContacts.end(); ++it) {
            if (*it >= 0)
                continue;

            quint32 iid = SeasideFilteredModel::internalId(it.key());
            QHash<quint32, SeasideCacheItem>::iterator cacheItem = m_people.find(iid);
            if (cacheItem != m_people.end()) {
                delete cacheItem->person;
                m_people.erase(cacheItem);
            }
        }
    }
    return true;
}

void SeasideCache::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_fetchTimer.timerId()) {
        fetchContacts();
    }

    if (event->timerId() == m_expiryTimer.timerId()) {
        m_expiryTimer.stop();
        instance = 0;
        deleteLater();
    }
}

void SeasideCache::contactsRemoved(const QList<ContactIdType> &)
{
    m_refreshRequired = true;
    requestUpdate();
}

void SeasideCache::updateContacts()
{
    QList<ContactIdType> contactIds;

    typedef QHash<quint32, SeasideCacheItem>::iterator iterator;
    for (iterator it = m_people.begin(); it != m_people.end(); ++it) {
        if (it->hasCompleteContact)
            contactIds.append(it->apiId());
    }

    updateContacts(contactIds);
}

void SeasideCache::fetchContacts()
{
    static const int WaitIntervalMs = 250;

    if (m_fetchRequest.isActive()) {
        // The current fetch is still active - we may as well continue to accumulate
        m_fetchTimer.start(WaitIntervalMs , this);
    } else {
        m_fetchTimer.stop();
        m_fetchPostponed.invalidate();

        // Fetch any changed contacts immediately
        if (m_contactsUpdated) {
            m_contactsUpdated = false;
            m_refreshRequired = true;
        }
        requestUpdate();
    }
}

void SeasideCache::updateContacts(const QList<ContactIdType> &contactIds)
{
    // Wait for new changes to be reported
    static const int PostponementIntervalMs = 500;

    // Maximum wait until we fetch all changes previously reported
    static const int MaxPostponementMs = 5000;

    m_contactsUpdated = true;
    m_changedContacts.append(contactIds);

    if (m_fetchPostponed.isValid()) {
        // We are waiting to accumulate further changes
        int remainder = MaxPostponementMs - m_fetchPostponed.elapsed();
        if (remainder > 0) {
            // We can postpone further
            m_fetchTimer.start(std::min(remainder, PostponementIntervalMs), this);
        }
    } else {
        // Wait for further changes before we query for the ones we have now
        m_fetchPostponed.restart();
        m_fetchTimer.start(PostponementIntervalMs, this);
    }
}

void SeasideCache::contactsAvailable()
{
    QContactAbstractRequest *request = static_cast<QContactAbstractRequest *>(sender());

    QList<QContact> contacts;
    if (request == &m_fetchByIdRequest) {
        contacts = m_fetchByIdRequest.contacts();
    } else {
        contacts = m_fetchRequest.contacts();
    }

    if (m_fetchFilter == SeasideFilteredModel::FilterFavorites
            || m_fetchFilter == SeasideFilteredModel::FilterOnline
            || m_fetchFilter == SeasideFilteredModel::FilterAll) {
        // Part of an initial query.
        appendContacts(contacts);
    } else {
        // An update.
        QList<QChar> modifiedGroups;

        for (int i = m_resultsRead; i < contacts.count(); ++i) {
            QContact contact = contacts.at(i);
            ContactIdType apiId = SeasideFilteredModel::apiId(contact);
            quint32 iid = SeasideFilteredModel::internalId(contact);

            SeasideCacheItem &item = m_people[iid];
            QContactName oldName = item.contact.detail<QContactName>();
            QContactName newName = contact.detail<QContactName>();
            QChar oldNameGroup;

            if (m_fetchFilter == SeasideFilteredModel::FilterAll)
                oldNameGroup = nameGroupForCacheItem(&item);

#ifdef USING_QTPIM
            if (newName.value<QString>(QContactName__FieldCustomLabel).isEmpty()) {
#else
            if (newName.customLabel().isEmpty()) {
#endif
#ifdef USING_QTPIM
                newName.setValue(QContactName__FieldCustomLabel, oldName.value(QContactName__FieldCustomLabel));
#else
                newName.setCustomLabel(oldName.customLabel());
#endif
                contact.saveDetail(&newName);
            }

            const bool roleDataChanged = newName != oldName
                    || contact.detail<QContactAvatar>().imageUrl() != item.contact.detail<QContactAvatar>().imageUrl();

            item.contact = contact;
            item.hasCompleteContact = true;
            if (item.person) {
                item.person->setContact(contact);
                item.person->setComplete(true);
                item.filterKey.clear();
            }

             QList<QContactPhoneNumber> phoneNumbers = contact.details<QContactPhoneNumber>();
             for (int j = 0; j < phoneNumbers.count(); ++j) {
                 m_phoneNumberIds[phoneNumbers.at(j).number()] = iid;
             }

             if (m_fetchFilter == SeasideFilteredModel::FilterAll) {
                 // do this even if !roleDataChanged as name groups are affected by other display label changes
                 QChar newNameGroup = nameGroupForCacheItem(&item);
                 if (newNameGroup != oldNameGroup) {
                     addToContactNameGroup(newNameGroup, &modifiedGroups);
                     removeFromContactNameGroup(oldNameGroup, &modifiedGroups);
                 }
             }

             if (roleDataChanged) {
                instance->updateContactData(apiId, SeasideFilteredModel::FilterFavorites);
                instance->updateContactData(apiId, SeasideFilteredModel::FilterOnline);
                instance->updateContactData(apiId, SeasideFilteredModel::FilterAll);
             }
        }
        m_resultsRead = contacts.count();
        notifyNameGroupsChanged(modifiedGroups);
    }
}

void SeasideCache::addToContactNameGroup(const QChar &group, QList<QChar> *modifiedGroups)
{
    if (!group.isNull()) {
        m_contactNameGroups[group] += 1;
        if (modifiedGroups && !m_nameGroupChangeListeners.isEmpty())
            modifiedGroups->append(group);
    }
}

void SeasideCache::removeFromContactNameGroup(const QChar &group, QList<QChar> *modifiedGroups)
{
    if (!group.isNull() && m_contactNameGroups.contains(group)) {
        m_contactNameGroups[group] -= 1;
        if (modifiedGroups && !m_nameGroupChangeListeners.isEmpty())
            modifiedGroups->append(group);
    }
}

void SeasideCache::notifyNameGroupsChanged(const QList<QChar> &groups)
{
    if (groups.isEmpty() || m_nameGroupChangeListeners.isEmpty())
        return;

    QHash<QChar, int> updates;
    for (int i = 0; i < groups.count(); ++i)
        updates[groups[i]] = m_contactNameGroups[groups[i]];

    for (int i = 0; i < m_nameGroupChangeListeners.count(); ++i)
        m_nameGroupChangeListeners[i]->nameGroupsUpdated(updates);
}

void SeasideCache::contactIdsAvailable()
{
    synchronizeList(
            this,
            m_contacts[m_fetchFilter],
            m_cacheIndex,
            m_contactIdRequest.ids(),
            m_queryIndex);
}

void SeasideCache::relationshipsAvailable()
{
#ifdef USING_QTPIM
    static const QString aggregatesRelationship = QContactRelationship::Aggregates();
#else
    static const QString aggregatesRelationship = QContactRelationship::Aggregates;
#endif

    foreach (const QContactRelationship &rel, m_relationshipsFetchRequest.relationships()) {
        if (rel.relationshipType() == aggregatesRelationship) {
#ifdef USING_QTPIM
            m_constituentIds.append(SeasideFilteredModel::apiId(rel.second()));
#else
            m_constituentIds.append(rel.second().localId());
#endif
        }
    }
}

void SeasideCache::finalizeUpdate(SeasideFilteredModel::FilterType filter)
{
    const QList<ContactIdType> queryIds = m_contactIdRequest.ids();
    QVector<ContactIdType> &cacheIds = m_contacts[filter];

    if (m_cacheIndex < cacheIds.count())
        removeRange(filter, m_cacheIndex, cacheIds.count() - m_cacheIndex);

    if (m_queryIndex < queryIds.count()) {
        const int count = queryIds.count() - m_queryIndex;
        if (count)
            insertRange(filter, cacheIds.count(), count, queryIds, m_queryIndex);
    }

    m_cacheIndex = 0;
    m_queryIndex = 0;
}

void SeasideCache::removeRange(
        SeasideFilteredModel::FilterType filter, int index, int count)
{
    QVector<ContactIdType> &cacheIds = m_contacts[filter];
    QList<SeasideFilteredModel *> &models = m_models[filter];
    QList<QChar> modifiedNameGroups;

    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceAboutToRemoveItems(index, index + count - 1);

    for (int i = 0; i < count; ++i) {
        if (filter == SeasideFilteredModel::FilterAll) {
            m_expiredContacts[cacheIds.at(index)] -= 1;

            removeFromContactNameGroup(nameGroupForCacheItem(cacheItemById(cacheIds.at(index))), &modifiedNameGroups);
        }

        cacheIds.remove(index);
    }

    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceItemsRemoved();

    notifyNameGroupsChanged(modifiedNameGroups);
}

int SeasideCache::insertRange(
        SeasideFilteredModel::FilterType filter,
        int index,
        int count,
        const QList<ContactIdType> &queryIds,
        int queryIndex)
{
    QVector<ContactIdType> &cacheIds = m_contacts[filter];
    QList<SeasideFilteredModel *> &models = m_models[filter];
    QList<QChar> modifiedNameGroups;

    const ContactIdType selfId = m_manager.selfContactId();

    int end = index + count - 1;
    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceAboutToInsertItems(index, end);

    for (int i = 0; i < count; ++i) {
        if (queryIds.at(queryIndex + i) == selfId)
            continue;

        if (filter == SeasideFilteredModel::FilterAll) {
            m_expiredContacts[queryIds.at(queryIndex + i)] += 1;

            addToContactNameGroup(nameGroupForCacheItem(cacheItemById(queryIds.at(queryIndex + i))), &modifiedNameGroups);
        }

        cacheIds.insert(index + i, queryIds.at(queryIndex + i));
    }

    for (int i = 0; i < models.count(); ++i)
        models[i]->sourceItemsInserted(index, end);

    notifyNameGroupsChanged(modifiedNameGroups);

    return end - index + 1;
}

void SeasideCache::appendContacts(const QList<QContact> &contacts)
{
    if (!contacts.isEmpty()) {
        QVector<ContactIdType> &cacheIds = m_contacts[m_fetchFilter];
        QList<SeasideFilteredModel *> &models = m_models[m_fetchFilter];

        cacheIds.reserve(contacts.count());

        const int begin = cacheIds.count();
        int end = cacheIds.count() + contacts.count() - m_appendIndex - 1;

        if (begin <= end) {
            for (int i = 0; i < models.count(); ++i)
                models.at(i)->sourceAboutToInsertItems(begin, end);

            for (; m_appendIndex < contacts.count(); ++m_appendIndex) {
                QContact contact = contacts.at(m_appendIndex);
                ContactIdType apiId = SeasideFilteredModel::apiId(contact);
                quint32 iid = SeasideFilteredModel::internalId(contact);

                cacheIds.append(apiId);
                SeasideCacheItem &cacheItem = m_people[iid];
                cacheItem.contact = contact;
                cacheItem.filterKey = QStringList();

                if (m_fetchFilter == SeasideFilteredModel::FilterAll)
                    addToContactNameGroup(nameGroupForCacheItem(&cacheItem), 0);

                foreach (const QContactPhoneNumber &phoneNumber, contact.details<QContactPhoneNumber>()) {
                    QString normalizedNumber = Normalization::normalizePhoneNumber(phoneNumber.number());
                    m_phoneNumberIds[normalizedNumber] = iid;
                }
            }

            for (int i = 0; i < models.count(); ++i)
                models.at(i)->sourceItemsInserted(begin, end);

            if (!m_nameGroupChangeListeners.isEmpty())
                notifyNameGroupsChanged(m_contactNameGroups.keys());
        }
    }
}

void SeasideCache::requestStateChanged(QContactAbstractRequest::State state)
{
    if (state != QContactAbstractRequest::FinishedState)
        return;

    QContactAbstractRequest *request = static_cast<QContactAbstractRequest *>(sender());

    if (request == &m_relationshipsFetchRequest) {
        if (m_constituentIds.isEmpty()) {
            // We didn't find any constituents - report the empty list
            QContactId aggregateId = m_contactsToFetchConstituents.takeFirst();
#ifdef USING_QTPIM
            SeasidePerson *person = personById(aggregateId);
#else
            SeasidePerson *person = personById(aggregateId.localId());
#endif
            person->setConstituents(QList<int>());
            emit person->constituentsChanged();
        }
    } else if (request == &m_fetchByIdRequest) {
        if (!m_constituentIds.isEmpty()) {
            // Report these results
            QContactId aggregateId = m_contactsToFetchConstituents.takeFirst();
#ifdef USING_QTPIM
            SeasidePerson *person = personById(aggregateId);
#else
            SeasidePerson *person = personById(aggregateId.localId());
#endif

            QList<int> constituentIds;
            foreach (const ContactIdType &id, m_constituentIds) {
                constituentIds.append(SeasideFilteredModel::internalId(id));
            }
            m_constituentIds.clear();

            person->setConstituents(constituentIds);
            emit person->constituentsChanged();
        }
    }

    if (m_fetchFilter == SeasideFilteredModel::FilterFavorites) {
        // Next, query for all contacts
        m_fetchFilter = SeasideFilteredModel::FilterAll;

        if (!isPopulated(SeasideFilteredModel::FilterFavorites)) {
            qDebug() << "Favorites queried in" << m_timer.elapsed() << "ms";
            m_appendIndex = 0;
            m_fetchRequest.setFilter(QContactFilter());
            m_fetchRequest.start();
            makePopulated(SeasideFilteredModel::FilterFavorites);
        } else {
            finalizeUpdate(SeasideFilteredModel::FilterFavorites);
            m_contactIdRequest.setFilter(QContactFilter());
            m_contactIdRequest.start();
        }
    } else if (m_fetchFilter == SeasideFilteredModel::FilterAll) {
        // Next, query for online contacts
        m_fetchFilter = SeasideFilteredModel::FilterOnline;

        if (!isPopulated(SeasideFilteredModel::FilterAll)) {
            qDebug() << "All queried in" << m_timer.elapsed() << "ms";
            // Not correct, but better than nothing...
            m_appendIndex = 0;
            m_fetchRequest.setFilter(QContactGlobalPresence::match(QContactPresence::PresenceAvailable));
            m_fetchRequest.start();
            makePopulated(SeasideFilteredModel::FilterNone);
            makePopulated(SeasideFilteredModel::FilterAll);
        } else {
            finalizeUpdate(SeasideFilteredModel::FilterAll);
            m_contactIdRequest.setFilter(QContactGlobalPresence::match(QContactPresence::PresenceAvailable));
            m_contactIdRequest.start();
        }
    } else if (m_fetchFilter == SeasideFilteredModel::FilterOnline) {
        m_fetchFilter = SeasideFilteredModel::FilterNone;

        if (m_updatesPending) {
            QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
        }

        if (!isPopulated(SeasideFilteredModel::FilterOnline)) {
            qDebug() << "Online queried in" << m_timer.elapsed() << "ms";
            m_fetchRequest.setFetchHint(QContactFetchHint());
            makePopulated(SeasideFilteredModel::FilterOnline);
        } else {
            finalizeUpdate(SeasideFilteredModel::FilterOnline);
        }
    } else if (m_fetchFilter == SeasideFilteredModel::FilterNone) {
        // Result of a specific query
        if (m_updatesPending) {
            QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
        }
    }
}

void SeasideCache::makePopulated(SeasideFilteredModel::FilterType filter)
{
    m_populated |= (1 << filter);

    QList<SeasideFilteredModel *> &models = m_models[filter];
    for (int i = 0; i < models.count(); ++i)
        models.at(i)->makePopulated();
}

void SeasideCache::displayLabelOrderChanged()
{
#ifdef HAS_MLITE
    QVariant displayLabelOrder = m_displayLabelOrderConf.value();
    if (displayLabelOrder.isValid() && displayLabelOrder.toInt() != m_displayLabelOrder) {
        m_displayLabelOrder = SeasideFilteredModel::DisplayLabelOrder(displayLabelOrder.toInt());
        QContactSortOrder firstNameOrder;
        setDetailType<QContactName>(firstNameOrder, QContactName::FieldFirstName);
        firstNameOrder.setCaseSensitivity(Qt::CaseInsensitive);
        firstNameOrder.setDirection(Qt::AscendingOrder);
        firstNameOrder.setBlankPolicy(QContactSortOrder::BlanksFirst);

        QContactSortOrder secondNameOrder;
        setDetailType<QContactName>(secondNameOrder, QContactName::FieldLastName);
        secondNameOrder.setCaseSensitivity(Qt::CaseInsensitive);
        secondNameOrder.setDirection(Qt::AscendingOrder);
        secondNameOrder.setBlankPolicy(QContactSortOrder::BlanksFirst);

        QList<QContactSortOrder> sorting = m_displayLabelOrder == SeasideFilteredModel::FirstNameFirst
                ? (QList<QContactSortOrder>() << firstNameOrder << secondNameOrder)
                : (QList<QContactSortOrder>() << secondNameOrder << firstNameOrder);

        m_fetchRequest.setSorting(sorting);
        m_contactIdRequest.setSorting(sorting);

        typedef QHash<quint32, SeasideCacheItem>::iterator iterator;
        for (iterator it = m_people.begin(); it != m_people.end(); ++it) {
            if (it->person) {
                it->person->recalculateDisplayLabel(SeasideFilteredModel::DisplayLabelOrder(m_displayLabelOrder));
                it->contact = it->person->contact();
            } else {
                QContactName name = it->contact.detail<QContactName>();
#ifdef USING_QTPIM
                name.setValue(QContactName__FieldCustomLabel, SeasidePerson::generateDisplayLabel(it->contact));
#else
                name.setCustomLabel(SeasidePerson::generateDisplayLabel(it->contact));
#endif
                it->contact.saveDetail(&name);
            }
        }

        for (int i = 0; i < SeasideFilteredModel::FilterTypesCount; ++i) {
            for (int j = 0; j < m_models[i].count(); ++j)
                m_models[i].at(j)->updateDisplayLabelOrder();
        }

        m_refreshRequired = true;
        requestUpdate();
    }
#endif
}

int SeasideCache::importContacts(const QString &path)
{
    QFile vcf(path);
    if (!vcf.open(QIODevice::ReadOnly)) {
        qWarning() << Q_FUNC_INFO << "Cannot open " << path;
        return 0;
    }

    // TODO: thread
    QVersitReader reader(&vcf);
    reader.startReading();
    reader.waitForFinished();

    QVersitContactImporter importer;
    importer.importDocuments(reader.results());

    QList<QContact> newContacts = importer.contacts();

    instance->m_contactsToCreate += newContacts;
    instance->requestUpdate();

    return newContacts.count();
}

QString SeasideCache::exportContacts()
{
    QVersitContactExporter exporter;

    QList<QContact> contacts;
    contacts.reserve(instance->m_people.count());

    QList<ContactIdType> contactsToFetch;
    contactsToFetch.reserve(instance->m_people.count());

    const quint32 selfId = SeasideFilteredModel::internalId(instance->m_manager.selfContactId());

    typedef QHash<quint32, SeasideCacheItem>::iterator iterator;
    for (iterator it = instance->m_people.begin(); it != instance->m_people.end(); ++it) {
        if (it.key() == selfId) {
            continue;
        } else if (it->hasCompleteContact) {
            contacts.append(it->contact);
        } else {
            contactsToFetch.append(SeasideFilteredModel::apiId(it.key()));
        }
    }

    if (!contactsToFetch.isEmpty()) {
        QList<QContact> fetchedContacts = instance->m_manager.contacts(contactsToFetch);
        contacts.append(fetchedContacts);
    }

    if (!exporter.exportContacts(contacts)) {
        qWarning() << Q_FUNC_INFO << "Failed to export contacts: " << exporter.errorMap();
        return QString();
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    QString baseDir;
    foreach (const QString &loc, QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation)) {
        baseDir = loc;
        break;
    }
#else
    const QString baseDir = QDesktopServices::storageLocation(QDesktopServices::DocumentsLocation);
#endif
    QFile vcard(baseDir
              + QDir::separator()
              + QDateTime::currentDateTime().toString("ss_mm_hh_dd_mm_yyyy")
              + ".vcf");

    if (!vcard.open(QIODevice::WriteOnly)) {
        qWarning() << "Cannot open " << vcard.fileName();
        return QString();
    }

    QVersitWriter writer(&vcard);
    if (!writer.startWriting(exporter.documents())) {
        qWarning() << Q_FUNC_INFO << "Can't start writing vcards " << writer.error();
        return QString();
    }

    // TODO: thread
    writer.waitForFinished();
    return vcard.fileName();
}

