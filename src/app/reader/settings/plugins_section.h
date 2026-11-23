#pragma once
#include "plugins/plugin_types.h"

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

class QLabel;
class QVBoxLayout;
class QWidget;

namespace kobalt::ui {
class CaptionButton;
class ChoiceModal;
class DescribedOption;
class ModalOverlay;
} // namespace kobalt::ui

namespace kobalt::reader {
// Settings > System > Plugins: the plugins switch, the choice to open lookups on
// plugin answers, one row per plugin folder or saved data, and a details dialog.
// Choices are a draft that applies when Settings closes. Which plugins load takes
// effect when the next book opens; plugin answers first takes effect at once.
class PluginsSection final : public QObject
{
public:
    // Settings makes the controls, so that they look like its other options and list actions.
    using MakeOption = std::function<ui::DescribedOption *(const char *name, const QString &label,
                                                           const QString &description)>;
    using MakeRow =
            std::function<ui::CaptionButton *(const QString &label, std::function<void()> action)>;

    // Adds its rows to `rows` on `page`; dialogs open over `overlayParent`.
    PluginsSection(QWidget *page, QVBoxLayout *rows, QWidget *overlayParent,
                   const QString &folderLabel, MakeOption makeOption, MakeRow makeRow);
    ~PluginsSection() override;

    // The switches that the open book loaded its plugins with.
    void setApplied(const plugins::Switches &switches);
    // The draft, read from and written to preferences around Settings.
    void setDraft(const plugins::Switches &switches);
    plugins::Switches draft() const;
    void setStatuses(const QVector<plugins::PluginStatus> &statuses);
    // Plugins are off because a plugin thread from an earlier book still runs.
    void setBlocked(bool blocked);
    // Removes a plugin's saved data and reports the result on the main thread.
    std::function<void(const QString &id, std::function<void(bool ok, QString error)> done)>
            deleteRequested;
    // A plugin row or a dialog made after the page was built; Settings gives it the
    // reader's text and theme.
    std::function<void(QWidget *)> widgetCreated;
    void dismissDialogs();

    ui::DescribedOption *usePluginsOption() const { return useOption; }

    ui::DescribedOption *answersFirstOption() const { return firstOption; }

private:
    void rebuild();
    void refreshStatuses();
    QString statusText(const plugins::PluginStatus &status) const;
    bool drafted(const QString &id) const;
    void openDetails(const plugins::PluginStatus &status);
    void confirmDelete(const plugins::PluginStatus &status);

    QWidget *page, *overlayParent;
    QString folderLabel;
    MakeRow makeRow;
    QVBoxLayout *rows;
    ui::DescribedOption *useOption, *firstOption;
    QLabel *hint, *blockedLabel;
    QVector<ui::CaptionButton *> pluginRows;
    QVector<plugins::PluginStatus> statuses;
    plugins::Switches applied, pending;
    bool blocked = false;
    QPointer<ui::ModalOverlay> details;
    QPointer<ui::ChoiceModal> confirmation;
};
} // namespace kobalt::reader
