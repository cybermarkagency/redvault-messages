// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#pragma once

#include <QAudioFormat>
#include <QImage>
#include <QObject>
#include <QQuickImageProvider>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <optional>
#include <vector>

#include "redvault/studio/audio_dsp.hpp"
#include "redvault/studio/image_edit.hpp"

class QAudioDecoder;
class QAudioSource;
class QIODevice;

// The Stories creation studio on the desktop, as on Android and the web: a photo editor, collages, voice stories with
// trim and effects, subtitles, and the motion of animated words. The maths is desktop/core/studio (pinned to the
// web's vectors); this draws it with Qt and hands QML what it needs. Everything stays on this computer: edits and
// recordings are made here and written only to this app's private cache folder until a story is shared.

namespace redvault::app {

/// Small helpers QML calls: motion frames, subtitle clocks and files, private scratch files.
class StudioTools : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    /// Windows asked for less motion (Settings, Accessibility, "Animation effects" off): words and emoji stay still.
    Q_PROPERTY(bool reducedMotion READ reducedMotion CONSTANT)

public:
    explicit StudioTools(QObject* parent = nullptr) : QObject(parent) {}

    bool reducedMotion() const;

    /// {scale, lift, alpha} of an animated overlay `ms` after it started ("" or unknown: still).
    Q_INVOKABLE QVariantMap motionFrame(const QString& anim, double ms) const;
    /// Still, pulse, bounce, fade and round again.
    Q_INVOKABLE QString nextMotion(const QString& anim) const;
    Q_INVOKABLE QString motionLabel(const QString& anim) const;

    /// "1:23.4".
    Q_INVOKABLE QString formatClock(double ms) const;
    /// Milliseconds, or -1 if it isn't a time.
    Q_INVOKABLE double parseClock(const QString& text) const;
    /// Subtitles from an .srt file: [{startMs, endMs, text}] (empty if none could be read).
    Q_INVOKABLE QVariantList readSrt(const QUrl& file) const;
    /// Typed lines (one per line of `text`) spread evenly over `durationMs`.
    Q_INVOKABLE QVariantList evenCaptions(const QString& text, double durationMs) const;
    /// The lines as every app will read them: cleaned, in time order, those that don't fit dropped.
    Q_INVOKABLE QVariantList checkCaptions(const QVariantList& captions) const;

    /// A new file in this app's private studio folder (removed when the app next starts): for the camera.
    Q_INVOKABLE QUrl scratchFile(const QString& extension) const;
    /// The same as a path (the camera's photo capture wants one), and back.
    Q_INVOKABLE QString scratchPath(const QString& extension) const { return scratchFile(extension).toLocalFile(); }
    Q_INVOKABLE QUrl fileUrl(const QString& path) const { return QUrl::fromLocalFile(path); }
    /// Removes the studio's scratch files (at start, and when the composer closes).
    Q_INVOKABLE void clearScratch() const;

    static QString scratchDir();
    static QUrl newScratchFile(const QString& extension);
};

/// Serves the editors' live previews to QML: image://studio/<key>/<revision>.
class StudioImageProvider : public QQuickImageProvider {
public:
    StudioImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

    static void put(const QString& key, const QImage& image);
    static void drop(const QString& key);
};

/// Draws an edit with Qt: turn and flip, crop, colour, blur, then the drawing on its own layer (the eraser takes away drawing, never the photo).
QImage renderEdit(const QImage& source, const studio::EditState& state, int maxSide);
/// A collage of `photos` (already loaded) in `layoutId`.
QImage renderCollage(const std::vector<QImage>& photos, const QString& layoutId, double gap, quint32 background, int width, int height);
/// A fresh JPEG in the scratch folder (on black, so transparent corners don't turn into noise); empty URL on failure.
QUrl saveJpeg(const QImage& image);

/// The photo editor: crop (free and fixed ratios), rotate, flip, brightness, contrast, saturation, blur, eight looks, drawing and erasing, undo and redo.
class PhotoEditor : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY sourceChanged)
    Q_PROPERTY(QString error READ error NOTIFY sourceChanged)
    /// The live preview (changes with every edit).
    Q_PROPERTY(QString previewUrl READ previewUrl NOTIFY changed)
    /// While cropping, the preview is the whole turned picture and the crop is drawn over it.
    Q_PROPERTY(bool cropping READ cropping WRITE setCropping NOTIFY changed)
    Q_PROPERTY(int rotation READ rotation NOTIFY changed)
    Q_PROPERTY(bool flipX READ flipX NOTIFY changed)
    /// {x, y, w, h} as fractions of the turned picture; the whole picture when there is no crop.
    Q_PROPERTY(QVariantMap crop READ crop NOTIFY changed)
    Q_PROPERTY(bool hasCrop READ hasCrop NOTIFY changed)
    Q_PROPERTY(QString filter READ filter NOTIFY changed)
    Q_PROPERTY(double brightness READ brightness NOTIFY changed)
    Q_PROPERTY(double contrast READ contrast NOTIFY changed)
    Q_PROPERTY(double saturation READ saturation NOTIFY changed)
    Q_PROPERTY(double blur READ blur NOTIFY changed)
    Q_PROPERTY(int strokeCount READ strokeCount NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(bool edited READ edited NOTIFY changed)
    /// The preview's shape (width / height), for laying it out.
    Q_PROPERTY(double previewAspect READ previewAspect NOTIFY changed)
    /// [{id, label}] of the eight looks.
    Q_PROPERTY(QVariantList filters READ filters CONSTANT)
    Q_PROPERTY(QStringList penColors READ penColors CONSTANT)

public:
    explicit PhotoEditor(QObject* parent = nullptr);
    ~PhotoEditor() override;

    QUrl source() const { return source_; }
    void setSource(const QUrl& url);
    bool loaded() const { return !image_.isNull(); }
    QString error() const { return error_; }
    QString previewUrl() const;
    bool cropping() const { return cropping_; }
    void setCropping(bool on);
    int rotation() const { return state().rotation; }
    bool flipX() const { return state().flipX; }
    QVariantMap crop() const;
    bool hasCrop() const { return state().crop.has_value(); }
    QString filter() const { return QString::fromStdString(state().filter); }
    double brightness() const { return state().adjust.brightness; }
    double contrast() const { return state().adjust.contrast; }
    double saturation() const { return state().adjust.saturation; }
    double blur() const { return state().adjust.blur; }
    int strokeCount() const { return static_cast<int>(state().strokes.size()); }
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }
    bool edited() const { return studio::image::hasEdits(state()); }
    double previewAspect() const;
    QVariantList filters() const;
    QStringList penColors() const;

    Q_INVOKABLE void rotate(int quarterTurns);
    Q_INVOKABLE void flip();
    /// A centred crop of width / height `aspect`; 0 lets the crop be dragged freely from the whole picture; -1 removes it.
    Q_INVOKABLE void setAspect(double aspect);
    /// Dragging a crop handle ("move", "nw", "n", ...): begin, move by fractions of the picture, end (one undo step).
    Q_INVOKABLE void beginCropDrag();
    Q_INVOKABLE void dragCrop(const QString& handle, double dx, double dy);
    Q_INVOKABLE void endCropDrag();
    Q_INVOKABLE void setFilter(const QString& filter);
    /// A slider ("brightness", "contrast", "saturation", "blur"): begin, set, end (one undo step).
    Q_INVOKABLE void beginAdjust();
    Q_INVOKABLE void setAdjust(const QString& name, double value);
    Q_INVOKABLE void endAdjust();
    /// Drawing: x and y are fractions of the finished picture; `size` a fraction of its width.
    Q_INVOKABLE void beginStroke(double x, double y, const QString& color, double size, bool eraser);
    Q_INVOKABLE void extendStroke(double x, double y);
    Q_INVOKABLE void endStroke();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    /// Back to the photo as it was.
    Q_INVOKABLE void resetAll();
    /// The finished photo as a fresh JPEG (at most 1920 on its long side) in the scratch folder; empty if it couldn't be made.
    Q_INVOKABLE QUrl save();

signals:
    void sourceChanged();
    void changed();

private:
    const studio::EditState& state() const { return history_.present(); }
    void push(studio::EditState next);
    void redraw();

    QUrl source_;
    QImage image_;    // the picture, at most 2400 on its long side
    QImage preview_;  // the picture at preview size (geometry and colour are cheap here)
    QString error_;
    QString key_;
    int revision_ = 0;
    bool cropping_ = false;
    studio::EditHistory<studio::EditState> history_;
    std::optional<studio::EditState> dragStart_;
    std::optional<studio::Stroke> stroke_;
};

/// Collages: two to four photos in one of six layouts, with spacing and a background.
class CollageMaker : public QObject {
    Q_OBJECT
    QML_ELEMENT
    /// The photos chosen, in order: [file URL].
    Q_PROPERTY(QVariantList photos READ photos NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    /// The layouts for this many photos: [{id, label, cells: [{x, y, w, h}]}].
    Q_PROPERTY(QVariantList layouts READ layouts NOTIFY changed)
    Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY changed)
    Q_PROPERTY(double gap READ gap WRITE setGap NOTIFY changed)
    Q_PROPERTY(int background READ background WRITE setBackground NOTIFY changed)
    /// [{label, color}].
    Q_PROPERTY(QVariantList backgrounds READ backgrounds CONSTANT)
    Q_PROPERTY(QString previewUrl READ previewUrl NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)

public:
    explicit CollageMaker(QObject* parent = nullptr);
    ~CollageMaker() override;

    QVariantList photos() const;
    int count() const { return static_cast<int>(images_.size()); }
    QVariantList layouts() const;
    QString layout() const { return layout_; }
    void setLayout(const QString& id);
    double gap() const { return gap_; }
    void setGap(double gap);
    int background() const { return background_; }
    void setBackground(int index);
    QVariantList backgrounds() const;
    QString previewUrl() const;
    bool ready() const { return count() >= 2; }
    QString error() const { return error_; }

    /// Adds photos (up to four in all); ones that can't be opened are skipped with a message.
    Q_INVOKABLE void addPhotos(const QList<QUrl>& files);
    Q_INVOKABLE void removePhoto(int index);
    /// Moves a photo one place earlier (-1) or later (1).
    Q_INVOKABLE void movePhoto(int index, int by);
    Q_INVOKABLE void clear();
    /// The collage as a fresh 1080 x 1920 JPEG in the scratch folder.
    Q_INVOKABLE QUrl save();

signals:
    void changed();

private:
    void redraw();

    std::vector<QUrl> files_;
    std::vector<QImage> images_;
    QString layout_;
    double gap_;
    int background_ = 0;
    QString error_;
    QString key_;
    int revision_ = 0;
};

/// Voice stories: record from the microphone (or open a sound file), trim, pick one of five effects, listen, and finish as a WAV.
class VoiceStudio : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool recording READ recording NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(double elapsedMs READ elapsedMs NOTIFY levelChanged)
    /// 0 to 1, the loudness right now (while recording).
    Q_PROPERTY(double level READ level NOTIFY levelChanged)
    Q_PROPERTY(bool hasSound READ hasSound NOTIFY stateChanged)
    /// The recording's whole length.
    Q_PROPERTY(double durationMs READ durationMs NOTIFY stateChanged)
    Q_PROPERTY(double trimStartMs READ trimStartMs NOTIFY editChanged)
    Q_PROPERTY(double trimEndMs READ trimEndMs NOTIFY editChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY editChanged)
    /// [{id, label}] of the six (none and five effects).
    Q_PROPERTY(QVariantList filters READ filters CONSTANT)
    /// 48 bars of the whole recording, for the trim view.
    Q_PROPERTY(QVariantList waveform READ waveform NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    Q_PROPERTY(qint64 maxMs READ maxMs CONSTANT)

public:
    static constexpr qint64 kMaxMs = 5 * 60 * 1000;
    static constexpr qint64 kMinMs = 700;

    explicit VoiceStudio(QObject* parent = nullptr);
    ~VoiceStudio() override;

    bool recording() const { return source_ != nullptr; }
    bool busy() const { return decoder_ != nullptr; }
    double elapsedMs() const;
    double level() const { return level_; }
    bool hasSound() const { return !sound_.samples.empty(); }
    double durationMs() const { return static_cast<double>(studio::audio::durationMs(sound_)); }
    double trimStartMs() const { return trimStart_; }
    double trimEndMs() const { return trimEnd_; }
    QString filter() const { return QString::fromUtf8(studio::filterWire(filter_).data()); }
    void setFilter(const QString& wire);
    QVariantList filters() const;
    QVariantList waveform() const { return waveform_; }
    QString error() const { return error_; }
    qint64 maxMs() const { return kMaxMs; }

    /// Records from the system's default microphone (up to 5 minutes).
    Q_INVOKABLE void startRecording();
    Q_INVOKABLE void stopRecording();
    /// Opens a sound file (WAV here; MP3, M4A, OGG and others through the system's decoder where there is one).
    Q_INVOKABLE void openFile(const QUrl& file);
    Q_INVOKABLE void setTrim(double startMs, double endMs);
    Q_INVOKABLE void clear();
    /// The voice as it will be shared (trimmed, with its effect), as a WAV to listen to; empty URL if there's nothing.
    Q_INVOKABLE QUrl preview();
    /// The finished voice: {url, durationMs, waveform} (url empty with `error` set if it is too short or too long).
    Q_INVOKABLE QVariantMap finish();
    /// For tests and the probe: a sound made elsewhere.
    void setSound(studio::Mono sound);

signals:
    void stateChanged();
    void levelChanged();
    void editChanged();
    /// Recording stopped by itself at 5 minutes.
    void limitReached();

private:
    void onAudio();
    void stopSource(bool keep);
    void soundChanged();

    studio::Mono sound_;
    std::vector<float> incoming_;
    QAudioFormat format_;
    QAudioSource* source_ = nullptr;
    QIODevice* device_ = nullptr;
    QAudioDecoder* decoder_ = nullptr;
    std::vector<std::vector<float>> decoded_;
    double decodedRate_ = 0;
    double level_ = 0;
    double trimStart_ = 0;
    double trimEnd_ = 0;
    studio::VoiceFilter filter_ = studio::VoiceFilter::None;
    QVariantList waveform_;
    QString error_;
};

}  // namespace redvault::app
