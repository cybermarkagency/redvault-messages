// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Winchester Marcelo Tuazon, trading as RedVault Systems

#include "studio_tools.hpp"

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioDevice>
#include <QAudioSource>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QTransform>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "redvault/model/stories.hpp"
#include "redvault/studio/captions.hpp"
#include "redvault/studio/collage.hpp"
#include "redvault/studio/story_motion.hpp"

namespace redvault::app {

namespace st = model::stories;
namespace img = studio::image;

namespace {

QMutex& previewsMutex() {
    static QMutex m;
    return m;
}

QHash<QString, QImage>& previews() {
    static QHash<QString, QImage> h;
    return h;
}

QString newKey() {
    return QUuid::createUuid().toString(QUuid::Id128);
}

QVariantList captionList(const std::vector<st::Caption>& list) {
    QVariantList out;
    for (const auto& c : list) {
        out.append(QVariantMap{{"startMs", static_cast<double>(c.startMs)}, {"endMs", static_cast<double>(c.endMs)}, {"text", QString::fromStdString(c.text)}});
    }
    return out;
}

/// The picture, read with its orientation applied and cut down to at most `maxSide` on its long side.
QImage loadPicture(const QString& path, int maxSide, QString* error) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
        if (error) *error = QStringLiteral("That photo couldn't be opened.");
        return {};
    }
    if (std::max(image.width(), image.height()) > maxSide) image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image.convertToFormat(QImage::Format_ARGB32);
}

/// Samples of any of Qt's formats as floats, mixed down to one channel.
void appendMono(const char* data, qsizetype bytes, const QAudioFormat& format, std::vector<float>& out, double* rms) {
    const int channels = std::max(1, format.channelCount());
    const int width = format.bytesPerSample();
    if (width <= 0) return;
    const qsizetype frames = bytes / (width * channels);
    double sum = 0;
    for (qsizetype f = 0; f < frames; ++f) {
        double mix = 0;
        for (int c = 0; c < channels; ++c) {
            const char* p = data + (f * channels + c) * width;
            double v = 0;
            switch (format.sampleFormat()) {
                case QAudioFormat::UInt8: v = (static_cast<unsigned char>(*p) - 128) / 128.0; break;
                case QAudioFormat::Int16: {
                    std::int16_t s;
                    std::memcpy(&s, p, 2);
                    v = s / 32768.0;
                    break;
                }
                case QAudioFormat::Int32: {
                    std::int32_t s;
                    std::memcpy(&s, p, 4);
                    v = s / 2147483648.0;
                    break;
                }
                case QAudioFormat::Float: {
                    float s;
                    std::memcpy(&s, p, 4);
                    v = s;
                    break;
                }
                default: break;
            }
            mix += v;
        }
        const double m = std::clamp(mix / channels, -1.0, 1.0);
        out.push_back(static_cast<float>(m));
        sum += m * m;
    }
    if (rms) *rms = frames > 0 ? std::sqrt(sum / static_cast<double>(frames)) : 0;
}

QUrl writeScratch(const std::string& bytes, const QString& extension) {
    const QUrl url = StudioTools::newScratchFile(extension);
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes.data(), static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size())) {
        file.close();
        QFile::remove(url.toLocalFile());
        return {};
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return url;
}

}  // namespace

// --- StudioTools ----------------------------------------------------------------------------------------------------

bool StudioTools::reducedMotion() const {
#ifdef _WIN32
    BOOL on = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0)) return on == FALSE;
#endif
    return false;
}

QVariantMap StudioTools::motionFrame(const QString& anim, double ms) const {
    const auto f = studio::motion::frame(anim.toStdString(), static_cast<std::int64_t>(ms));
    return {{"scale", f.scale}, {"lift", f.lift}, {"alpha", f.alpha}};
}

QString StudioTools::nextMotion(const QString& anim) const {
    return QString::fromStdString(studio::motion::next(anim.toStdString()));
}

QString StudioTools::motionLabel(const QString& anim) const {
    return QString::fromStdString(studio::motion::label(anim.toStdString()));
}

QString StudioTools::formatClock(double ms) const {
    return QString::fromStdString(studio::captions::formatClock(ms));
}

double StudioTools::parseClock(const QString& text) const {
    const auto ms = studio::captions::parseClock(text.toStdString());
    return ms ? static_cast<double>(*ms) : -1.0;
}

QVariantList StudioTools::readSrt(const QUrl& file) const {
    QFile in(file.toLocalFile());
    if (in.size() > 512 * 1024 || !in.open(QIODevice::ReadOnly)) return {};
    return captionList(studio::captions::parseSrt(in.readAll().toStdString()));
}

QVariantList StudioTools::evenCaptions(const QString& text, double durationMs) const {
    std::vector<std::string> lines;
    for (const auto& l : text.split(u'\n')) lines.push_back(l.toStdString());
    return captionList(studio::captions::evenCaptions(lines, static_cast<std::int64_t>(durationMs)));
}

QVariantList StudioTools::checkCaptions(const QVariantList& captions) const {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& v : captions) {
        const QVariantMap m = v.toMap();
        list.push_back({{"startMs", m.value("startMs").toDouble()}, {"endMs", m.value("endMs").toDouble()}, {"text", m.value("text").toString().toStdString()}});
    }
    return captionList(st::parseCaptions(list.dump()));
}

QString StudioTools::scratchDir() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/story-studio");
}

QUrl StudioTools::newScratchFile(const QString& extension) {
    const QString dir = scratchDir();
    QDir().mkpath(dir);
    return QUrl::fromLocalFile(dir + u'/' + newKey() + u'.' + extension);
}

QUrl StudioTools::scratchFile(const QString& extension) const {
    static const QStringList allowed = {"jpg", "mp4", "wav"};
    return newScratchFile(allowed.contains(extension) ? extension : QStringLiteral("bin"));
}

void StudioTools::clearScratch() const {
    QDir(scratchDir()).removeRecursively();
}

// --- Previews -------------------------------------------------------------------------------------------------------

QImage StudioImageProvider::requestImage(const QString& id, QSize* size, const QSize&) {
    const QString key = id.section(u'/', 0, 0);
    QMutexLocker lock(&previewsMutex());
    QImage image = previews().value(key);
    if (image.isNull()) {
        image = QImage(1, 1, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
    }
    if (size) *size = image.size();
    return image;
}

void StudioImageProvider::put(const QString& key, const QImage& image) {
    QMutexLocker lock(&previewsMutex());
    previews().insert(key, image);
}

void StudioImageProvider::drop(const QString& key) {
    QMutexLocker lock(&previewsMutex());
    previews().remove(key);
}

// --- Drawing --------------------------------------------------------------------------------------------------------

QImage renderEdit(const QImage& source, const studio::EditState& s, int maxSide) {
    if (source.isNull()) return {};
    const auto [rw, rh] = img::rotatedSize(source.width(), source.height(), s.rotation);
    // Turned and flipped (the flip mirrors the finished picture, after the turn).
    QImage turned(rw, rh, QImage::Format_ARGB32);
    turned.fill(Qt::transparent);
    {
        QPainter p(&turned);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QTransform t;
        t.translate(rw / 2.0, rh / 2.0);
        if (s.flipX) t.scale(-1, 1);
        t.rotate(s.rotation);
        t.translate(-source.width() / 2.0, -source.height() / 2.0);
        p.setTransform(t);
        p.drawImage(0, 0, source);
    }
    const auto crop = img::cropPixels(source.width(), source.height(), s);
    const auto [ow, oh] = img::outputSize(source.width(), source.height(), s, maxSide);
    QImage out(ow, oh, QImage::Format_ARGB32);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(QRect(0, 0, ow, oh), turned, QRect(crop.sx, crop.sy, std::min(crop.sw, rw - crop.sx), std::min(crop.sh, rh - crop.sy)));
    }
    const auto params = img::colorParams(s, oh);
    if (!params.neutral()) {
        auto* pixels = reinterpret_cast<std::uint32_t*>(out.bits());
        img::adjustArgb(pixels, static_cast<std::size_t>(ow) * static_cast<std::size_t>(oh), params, true);
        if (params.blurPx > 0.05) img::blurArgb(pixels, ow, oh, params.blurPx);
    }
    if (!s.strokes.empty()) {
        QImage layer(ow, oh, QImage::Format_ARGB32_Premultiplied);
        layer.fill(Qt::transparent);
        QPainter p(&layer);
        p.setRenderHint(QPainter::Antialiasing);
        for (const auto& stroke : s.strokes) {
            const double width = stroke.size * ow;
            p.setCompositionMode(stroke.eraser ? QPainter::CompositionMode_Clear : QPainter::CompositionMode_SourceOver);
            const QColor color(QString::fromStdString(img::penColor(stroke.color)));
            if (stroke.points.size() == 1) {
                p.setPen(Qt::NoPen);
                p.setBrush(color);
                p.drawEllipse(QPointF(stroke.points[0].x * ow, stroke.points[0].y * oh), width / 2, width / 2);
                continue;
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath path(QPointF(stroke.points[0].x * ow, stroke.points[0].y * oh));
            for (std::size_t i = 1; i < stroke.points.size(); ++i) path.lineTo(stroke.points[i].x * ow, stroke.points[i].y * oh);
            p.drawPath(path);
        }
        p.end();
        QPainter onto(&out);
        onto.drawImage(0, 0, layer);
    }
    return out;
}

QImage renderCollage(const std::vector<QImage>& photos, const QString& layoutId, double gap, quint32 background, int width, int height) {
    const auto* layout = studio::collage::layoutById(layoutId.toStdString());
    if (!layout || layout->cells.size() != photos.size()) return {};
    QImage out(width, height, QImage::Format_RGB32);
    out.fill(QColor::fromRgba(background));
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double scale = width / static_cast<double>(studio::collage::kWidth);
    const auto rects = studio::collage::cellRects(*layout, width, height, gap * scale);
    for (std::size_t i = 0; i < photos.size(); ++i) {
        const auto& r = rects[i];
        const QImage& image = photos[i];
        const auto f = studio::collage::fitCover(image.width(), image.height(), r.w, r.h);
        p.drawImage(QRectF(r.x, r.y, r.w, r.h), image, QRectF(f.sx, f.sy, f.sw, f.sh));
    }
    return out;
}

QUrl saveJpeg(const QImage& image) {
    if (image.isNull()) return {};
    // A transparent corner (the picture's own, or a drawing erased to nothing) would become noise in a JPEG: paint on black first.
    QImage flat(image.size(), QImage::Format_RGB32);
    flat.fill(Qt::black);
    {
        QPainter p(&flat);
        p.drawImage(0, 0, image);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    if (!flat.save(&buffer, "JPEG", 90)) return {};
    return writeScratch(std::string(jpeg.constData(), static_cast<std::size_t>(jpeg.size())), QStringLiteral("jpg"));
}

// --- PhotoEditor ----------------------------------------------------------------------------------------------------

namespace {
constexpr int kEditMaxSide = 2400;
constexpr int kPreviewMaxSide = 1000;
}  // namespace

PhotoEditor::PhotoEditor(QObject* parent) : QObject(parent), key_(newKey()) {}

PhotoEditor::~PhotoEditor() {
    StudioImageProvider::drop(key_);
}

void PhotoEditor::setSource(const QUrl& url) {
    if (url == source_) return;
    source_ = url;
    error_.clear();
    image_ = url.isLocalFile() ? loadPicture(url.toLocalFile(), kEditMaxSide, &error_) : QImage();
    preview_ = image_.isNull() ? QImage() : image_.scaled(kPreviewMaxSide, kPreviewMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    history_ = studio::EditHistory<studio::EditState>();
    dragStart_.reset();
    stroke_.reset();
    cropping_ = false;
    emit sourceChanged();
    redraw();
}

QString PhotoEditor::previewUrl() const {
    return image_.isNull() ? QString() : QStringLiteral("image://studio/%1/%2").arg(key_).arg(revision_);
}

void PhotoEditor::setCropping(bool on) {
    if (on == cropping_) return;
    cropping_ = on;
    redraw();
}

QVariantMap PhotoEditor::crop() const {
    const auto c = state().crop.value_or(studio::Rect{0, 0, 1, 1});
    return {{"x", c.x}, {"y", c.y}, {"w", c.w}, {"h", c.h}};
}

double PhotoEditor::previewAspect() const {
    if (preview_.isNull()) return 9.0 / 16.0;
    studio::EditState s = state();
    if (cropping_) s.crop.reset();
    const auto [w, h] = img::outputSize(preview_.width(), preview_.height(), s, kPreviewMaxSide);
    return static_cast<double>(w) / h;
}

QVariantList PhotoEditor::filters() const {
    QVariantList out;
    for (const auto& f : img::filters()) out.append(QVariantMap{{"id", QString::fromStdString(f)}, {"label", QString::fromStdString(img::filterLabel(f))}});
    return out;
}

QStringList PhotoEditor::penColors() const {
    QStringList out;
    for (const auto& c : st::kColors) out.append(QString::fromStdString(c));
    return out;
}

void PhotoEditor::push(studio::EditState next) {
    history_.push(std::move(next));
    redraw();
}

void PhotoEditor::redraw() {
    if (!preview_.isNull()) {
        studio::EditState s = state();
        if (cropping_) {
            // While cropping: the whole turned picture, with no drawing (drawing belongs to the finished picture).
            s.crop.reset();
            s.strokes.clear();
        }
        if (stroke_) s.strokes.push_back(*stroke_);
        StudioImageProvider::put(key_, renderEdit(preview_, s, kPreviewMaxSide));
        ++revision_;
    }
    emit changed();
}

void PhotoEditor::rotate(int quarterTurns) {
    const auto& s = state();
    push(img::withGeometry(s, img::rotateBy(s.rotation, quarterTurns), s.flipX, std::nullopt));
}

void PhotoEditor::flip() {
    const auto& s = state();
    push(img::withGeometry(s, s.rotation, !s.flipX, std::nullopt));
}

void PhotoEditor::setAspect(double aspect) {
    if (image_.isNull()) return;
    const auto& s = state();
    const auto [rw, rh] = img::rotatedSize(image_.width(), image_.height(), s.rotation);
    std::optional<studio::Rect> crop;
    if (aspect > 0) crop = img::aspectCrop(aspect, rw, rh);
    push(img::withGeometry(s, s.rotation, s.flipX, crop));
}

void PhotoEditor::beginCropDrag() {
    dragStart_ = state();
}

void PhotoEditor::dragCrop(const QString& handle, double dx, double dy) {
    const auto& s = state();
    const auto next = img::dragRect(s.crop.value_or(studio::Rect{0, 0, 1, 1}), handle.toStdString(), dx, dy);
    history_.replace(img::withGeometry(s, s.rotation, s.flipX, next));
    redraw();
}

void PhotoEditor::endCropDrag() {
    if (dragStart_) history_.commitFrom(*dragStart_);
    dragStart_.reset();
    emit changed();
}

void PhotoEditor::setFilter(const QString& filter) {
    const auto f = filter.toStdString();
    if (std::find(img::filters().begin(), img::filters().end(), f) == img::filters().end()) return;
    studio::EditState s = state();
    s.filter = f;
    push(std::move(s));
}

void PhotoEditor::beginAdjust() {
    dragStart_ = state();
}

void PhotoEditor::setAdjust(const QString& name, double value) {
    studio::EditState s = state();
    if (name == QStringLiteral("brightness")) s.adjust.brightness = std::clamp(value, -100.0, 100.0);
    else if (name == QStringLiteral("contrast")) s.adjust.contrast = std::clamp(value, -100.0, 100.0);
    else if (name == QStringLiteral("saturation")) s.adjust.saturation = std::clamp(value, -100.0, 100.0);
    else if (name == QStringLiteral("blur")) s.adjust.blur = std::clamp(value, 0.0, 100.0);
    else return;
    if (dragStart_) {
        history_.replace(std::move(s));
        redraw();
    } else {
        push(std::move(s));
    }
}

void PhotoEditor::endAdjust() {
    endCropDrag();
}

void PhotoEditor::beginStroke(double x, double y, const QString& color, double size, bool eraser) {
    std::string c = color.toStdString();
    if (std::find(st::kColors.begin(), st::kColors.end(), c) == st::kColors.end()) c = "white";
    stroke_ = img::newStroke(c, size, eraser, studio::Point{x, y});
    redraw();
}

void PhotoEditor::extendStroke(double x, double y) {
    if (!stroke_) return;
    const auto before = stroke_->points.size();
    stroke_ = img::extendStroke(*stroke_, studio::Point{x, y});
    if (stroke_->points.size() != before) redraw();
}

void PhotoEditor::endStroke() {
    if (!stroke_) return;
    const auto done = *stroke_;
    stroke_.reset();
    push(img::addStroke(state(), done));
}

void PhotoEditor::undo() {
    history_.undo();
    redraw();
}

void PhotoEditor::redo() {
    history_.redo();
    redraw();
}

void PhotoEditor::resetAll() {
    push(studio::EditState{});
}

QUrl PhotoEditor::save() {
    if (image_.isNull()) return {};
    return saveJpeg(renderEdit(image_, state(), st::kMaxImageSide));
}

// --- CollageMaker ---------------------------------------------------------------------------------------------------

CollageMaker::CollageMaker(QObject* parent) : QObject(parent), gap_(studio::collage::kDefaultGap), key_(newKey()) {}

CollageMaker::~CollageMaker() {
    StudioImageProvider::drop(key_);
}

QVariantList CollageMaker::photos() const {
    QVariantList out;
    for (const auto& f : files_) out.append(f);
    return out;
}

QVariantList CollageMaker::layouts() const {
    QVariantList out;
    for (const auto& l : studio::collage::layoutsFor(count())) {
        QVariantList cells;
        for (const auto& c : l.cells) cells.append(QVariantMap{{"x", c.x}, {"y", c.y}, {"w", c.w}, {"h", c.h}});
        out.append(QVariantMap{{"id", QString::fromStdString(l.id)}, {"label", QString::fromStdString(l.label)}, {"cells", cells}});
    }
    return out;
}

void CollageMaker::setLayout(const QString& id) {
    const auto* l = studio::collage::layoutById(id.toStdString());
    if (!l || static_cast<int>(l->cells.size()) != count() || id == layout_) return;
    layout_ = id;
    redraw();
}

void CollageMaker::setGap(double gap) {
    gap = std::clamp(gap, 0.0, static_cast<double>(studio::collage::kMaxGap));
    if (gap == gap_) return;
    gap_ = gap;
    redraw();
}

void CollageMaker::setBackground(int index) {
    if (index < 0 || index >= static_cast<int>(studio::collage::backgrounds().size()) || index == background_) return;
    background_ = index;
    redraw();
}

QVariantList CollageMaker::backgrounds() const {
    QVariantList out;
    for (const auto& b : studio::collage::backgrounds()) {
        out.append(QVariantMap{{"label", QString::fromStdString(b.label)}, {"color", QColor::fromRgba(b.argb).name()}});
    }
    return out;
}

QString CollageMaker::previewUrl() const {
    return ready() ? QStringLiteral("image://studio/%1/%2").arg(key_).arg(revision_) : QString();
}

void CollageMaker::addPhotos(const QList<QUrl>& files) {
    error_.clear();
    int skipped = 0;
    for (const auto& f : files) {
        if (count() >= studio::collage::kMaxPhotos) {
            error_ = QStringLiteral("A collage takes up to four photos.");
            break;
        }
        QString why;
        QImage image = f.isLocalFile() ? loadPicture(f.toLocalFile(), 1920, &why) : QImage();
        if (image.isNull()) {
            ++skipped;
            continue;
        }
        files_.push_back(f);
        images_.push_back(std::move(image));
    }
    if (skipped > 0 && error_.isEmpty()) error_ = skipped == 1 ? QStringLiteral("One photo couldn't be opened.") : QStringLiteral("Some photos couldn't be opened.");
    const auto options = studio::collage::layoutsFor(count());
    if (std::none_of(options.begin(), options.end(), [&](const auto& l) { return QString::fromStdString(l.id) == layout_; })) {
        layout_ = options.empty() ? QString() : QString::fromStdString(options.front().id);
    }
    redraw();
}

void CollageMaker::removePhoto(int index) {
    if (index < 0 || index >= count()) return;
    files_.erase(files_.begin() + index);
    images_.erase(images_.begin() + index);
    error_.clear();
    const auto options = studio::collage::layoutsFor(count());
    layout_ = options.empty() ? QString() : QString::fromStdString(options.front().id);
    redraw();
}

void CollageMaker::movePhoto(int index, int by) {
    const int to = index + by;
    if (index < 0 || index >= count() || to < 0 || to >= count()) return;
    std::swap(files_[static_cast<std::size_t>(index)], files_[static_cast<std::size_t>(to)]);
    std::swap(images_[static_cast<std::size_t>(index)], images_[static_cast<std::size_t>(to)]);
    redraw();
}

void CollageMaker::clear() {
    files_.clear();
    images_.clear();
    layout_.clear();
    error_.clear();
    redraw();
}

void CollageMaker::redraw() {
    if (ready() && !layout_.isEmpty()) {
        // The live preview: a third of the real size.
        StudioImageProvider::put(key_, renderCollage(images_, layout_, gap_, studio::collage::backgrounds()[static_cast<std::size_t>(background_)].argb,
                                                     studio::collage::kWidth / 3, studio::collage::kHeight / 3));
        ++revision_;
    }
    emit changed();
}

QUrl CollageMaker::save() {
    if (!ready() || layout_.isEmpty()) return {};
    return saveJpeg(renderCollage(images_, layout_, gap_, studio::collage::backgrounds()[static_cast<std::size_t>(background_)].argb,
                                  studio::collage::kWidth, studio::collage::kHeight));
}

// --- VoiceStudio ----------------------------------------------------------------------------------------------------

VoiceStudio::VoiceStudio(QObject* parent) : QObject(parent) {}

VoiceStudio::~VoiceStudio() {
    stopSource(false);
    if (decoder_) {
        decoder_->stop();
        delete decoder_;
    }
}

double VoiceStudio::elapsedMs() const {
    const double rate = format_.sampleRate() > 0 ? format_.sampleRate() : 1;
    return recording() ? incoming_.size() / rate * 1000 : durationMs();
}

void VoiceStudio::setFilter(const QString& wire) {
    const auto f = studio::filterFromWire(wire.toStdString());
    if (!f || *f == filter_) return;
    filter_ = *f;
    emit editChanged();
}

QVariantList VoiceStudio::filters() const {
    QVariantList out;
    for (const auto f : studio::kVoiceFilters) {
        out.append(QVariantMap{{"id", QString::fromUtf8(studio::filterWire(f).data())}, {"label", QString::fromUtf8(studio::filterLabel(f).data())}});
    }
    return out;
}

void VoiceStudio::startRecording() {
    if (recording() || busy()) return;
    error_.clear();
    const QAudioDevice device = QMediaDevices::defaultAudioInput();
    if (device.isNull()) {
        error_ = QStringLiteral("No microphone was found. Connect one, or open a sound file instead.");
        emit stateChanged();
        return;
    }
    QAudioFormat format = device.preferredFormat();
    QAudioFormat wanted = format;
    wanted.setSampleFormat(QAudioFormat::Float);
    if (device.isFormatSupported(wanted)) format = wanted;
    format_ = format;
    incoming_.clear();
    source_ = new QAudioSource(device, format, this);
    device_ = source_->start();
    if (!device_) {
        stopSource(false);
        error_ = QStringLiteral("The microphone couldn't be opened. Check that this app may use it (Windows Settings, Privacy, Microphone).");
        emit stateChanged();
        return;
    }
    connect(device_, &QIODevice::readyRead, this, &VoiceStudio::onAudio);
    emit stateChanged();
}

void VoiceStudio::onAudio() {
    if (!device_) return;
    const QByteArray data = device_->readAll();
    double rms = 0;
    appendMono(data.constData(), data.size(), format_, incoming_, &rms);
    level_ = std::min(1.0, rms * 3);
    emit levelChanged();
    if (elapsedMs() >= kMaxMs) {
        stopSource(true);
        emit limitReached();
    }
}

void VoiceStudio::stopRecording() {
    if (recording()) stopSource(true);
}

void VoiceStudio::stopSource(bool keep) {
    if (!source_) return;
    if (device_ && keep) {
        // What arrived since the last read.
        const QByteArray rest = device_->readAll();
        appendMono(rest.constData(), rest.size(), format_, incoming_, nullptr);
    }
    source_->stop();
    source_->deleteLater();
    source_ = nullptr;
    device_ = nullptr;
    level_ = 0;
    if (!keep) {
        incoming_.clear();
        emit stateChanged();
        return;
    }
    studio::Mono sound{std::move(incoming_), static_cast<double>(format_.sampleRate())};
    incoming_.clear();
    if (static_cast<double>(studio::audio::durationMs(sound)) < kMinMs) {
        error_ = QStringLiteral("That recording is too short.");
        emit stateChanged();
        return;
    }
    setSound(std::move(sound));
}

void VoiceStudio::openFile(const QUrl& file) {
    if (recording() || busy() || !file.isLocalFile()) return;
    error_.clear();
    const QString path = file.toLocalFile();
    if (QFileInfo(path).size() > 200ll * 1024 * 1024) {
        error_ = QStringLiteral("That file is too large.");
        emit stateChanged();
        return;
    }
    if (QFileInfo(path).suffix().compare(QStringLiteral("wav"), Qt::CaseInsensitive) == 0) {
        QFile in(path);
        if (in.open(QIODevice::ReadOnly)) {
            const QByteArray all = in.readAll();
            if (auto sound = studio::audio::decodeWav(std::string_view(all.constData(), static_cast<std::size_t>(all.size())))) {
                setSound(std::move(*sound));
                return;
            }
        }
    }
    // Anything else (and WAVs this app didn't write): the system's decoder, as 32-bit floats.
    decoder_ = new QAudioDecoder(this);
    decoded_.clear();
    decodedRate_ = 0;
    QAudioFormat want;
    want.setSampleFormat(QAudioFormat::Float);
    want.setChannelCount(1);
    decoder_->setAudioFormat(want);
    decoder_->setSource(file);
    decoded_.push_back({});
    connect(decoder_, &QAudioDecoder::bufferReady, this, [this] {
        const QAudioBuffer buffer = decoder_->read();
        if (!buffer.isValid()) return;
        decodedRate_ = buffer.format().sampleRate();
        appendMono(buffer.constData<char>(), buffer.byteCount(), buffer.format(), decoded_[0], nullptr);
        if (decodedRate_ > 0 && decoded_[0].size() / decodedRate_ * 1000 > kMaxMs + 1000) decoder_->stop();
    });
    const auto done = [this](bool ok, const QString& why) {
        auto* d = decoder_;
        decoder_ = nullptr;
        d->deleteLater();
        if (!ok || decoded_.empty() || decoded_[0].empty() || decodedRate_ <= 0) {
            error_ = why.isEmpty() ? QStringLiteral("That file couldn't be opened as sound.") : why;
            decoded_.clear();
            emit stateChanged();
            return;
        }
        studio::Mono sound{std::move(decoded_[0]), decodedRate_};
        decoded_.clear();
        setSound(std::move(sound));
    };
    connect(decoder_, &QAudioDecoder::finished, this, [done] { done(true, {}); });
    connect(decoder_, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this, [this, done](QAudioDecoder::Error) {
        if (decoder_) done(false, QStringLiteral("That file couldn't be opened as sound."));
    });
    emit stateChanged();
    decoder_->start();
}

void VoiceStudio::setSound(studio::Mono sound) {
    sound_ = std::move(sound);
    trimStart_ = 0;
    trimEnd_ = std::min<double>(durationMs(), kMaxMs);
    if (durationMs() > kMaxMs) error_ = QStringLiteral("A voice story can be up to 5 minutes: the end is cut off. Move the trim to keep another part.");
    waveform_.clear();
    for (const int bar : studio::audio::waveformBars(sound_.samples, 48)) waveform_.append(bar);
    emit stateChanged();
    emit editChanged();
}

void VoiceStudio::setTrim(double startMs, double endMs) {
    const double total = durationMs();
    startMs = std::clamp(startMs, 0.0, total);
    endMs = std::clamp(endMs, startMs, total);
    if (startMs == trimStart_ && endMs == trimEnd_) return;
    trimStart_ = startMs;
    trimEnd_ = endMs;
    emit editChanged();
}

void VoiceStudio::clear() {
    stopSource(false);
    sound_ = studio::Mono{};
    waveform_.clear();
    trimStart_ = trimEnd_ = 0;
    filter_ = studio::VoiceFilter::None;
    error_.clear();
    emit stateChanged();
    emit editChanged();
}

QUrl VoiceStudio::preview() {
    if (!hasSound()) return {};
    const auto done = studio::audio::finishVoice(sound_, trimStart_, trimEnd_, filter_);
    return writeScratch(done.wav, QStringLiteral("wav"));
}

QVariantMap VoiceStudio::finish() {
    const auto refuse = [this](const QString& why) {
        error_ = why;
        emit stateChanged();
        return QVariantMap{{"url", QUrl()}, {"durationMs", 0}, {"waveform", QVariantList()}};
    };
    if (!hasSound()) return refuse(QStringLiteral("Record something first, or open a sound file."));
    const auto done = studio::audio::finishVoice(sound_, trimStart_, trimEnd_, filter_);
    if (done.durationMs < 300) return refuse(QStringLiteral("That is too short for a story. Keep a little more."));
    if (done.durationMs > st::kMaxAudioMs) return refuse(QStringLiteral("A voice story can be up to 5 minutes. Trim it first."));
    if (done.wav.size() > st::kMaxAudioBytes) return refuse(QStringLiteral("That sound is too large for a story (at most 24 MB)."));
    const QUrl url = writeScratch(done.wav, QStringLiteral("wav"));
    if (url.isEmpty()) return refuse(QStringLiteral("The voice couldn't be saved. Try again."));
    QVariantList bars;
    for (const int b : done.waveform) bars.append(b);
    error_.clear();
    emit stateChanged();
    return {{"url", url}, {"durationMs", static_cast<double>(done.durationMs)}, {"waveform", bars}};
}

}  // namespace redvault::app
