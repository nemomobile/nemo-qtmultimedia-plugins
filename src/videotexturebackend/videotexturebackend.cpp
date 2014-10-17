/*
 * Copyright (C) 2014 Jolla Ltd
 * Contact: Andrew den Exter <andrew.den.exter@jollamobile.com>
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

#include <private/qdeclarativevideooutput_backend_p.h>
#include <private/qdeclarativevideooutput_p.h>

#include <QGuiApplication>
#include <QMediaObject>
#include <QMediaService>
#include <QMutex>
#include <QResizeEvent>
#include <QQuickWindow>
#include <QSGGeometry>
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGTexture>

#include <qpa/qplatformnativeinterface.h>
#include <private/qgstreamerelementcontrol_p.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gst/interfaces/nemovideotexture.h>

#include <QThread>

#define EGL_SYNC_FENCE_KHR                      0x30F9

class GStreamerVideoTexture : public QSGDynamicTexture
{
    Q_OBJECT
public:
    GStreamerVideoTexture(GstElement *sink, EGLDisplay display);
    ~GStreamerVideoTexture();

    bool isActive() const;

    int textureId() const;
    QSize textureSize() const;
    void setTextureSize(const QSize &size);
    bool hasAlphaChannel() const;
    bool hasMipmaps() const;

    QRectF normalizedTextureSubRect() const;

    void bind();
    bool updateTexture();

    void invalidateTexture();

public slots:
    void releaseTexture();

private:
    GstElement *m_sink;
    EGLDisplay m_display;
    QRectF m_subRect;
    QSize m_textureSize;
    GLuint m_textureId;
    bool m_updated;
};

class GStreamerVideoMaterial : public QSGMaterial
{
public:
    GStreamerVideoMaterial(GStreamerVideoTexture *texture);

    QSGMaterialShader *createShader() const;
    QSGMaterialType *type() const;
    int compare(const QSGMaterial *other) const;

    void setTexture(GStreamerVideoTexture *texture);

private:
    friend class GStreamerVideoMaterialShader;
    friend class GStreamerVideoNode;

    GStreamerVideoTexture *m_texture;
};

class GStreamerVideoNode : public QSGGeometryNode
{
public:
    GStreamerVideoNode(GStreamerVideoTexture *texture);
    ~GStreamerVideoNode();

    void setBoundingRect(const QRectF &rect, int orientation, bool horizontalMirror, bool verticalMirror);
    void preprocess();

private:
    GStreamerVideoMaterial m_material;
    QSGGeometry m_geometry;
};

GStreamerVideoTexture::GStreamerVideoTexture(GstElement *sink, EGLDisplay display)
    : m_sink(sink)
    , m_display(display)
    , m_subRect(0, 0, 1, 1)
    , m_textureId(0)
    , m_updated(false)
{
    gst_object_ref(GST_OBJECT(m_sink));
}

GStreamerVideoTexture::~GStreamerVideoTexture()
{
    releaseTexture();

    invalidateTexture();

    if (m_sink) {
        gst_object_unref(GST_OBJECT(m_sink));
    }
}

int GStreamerVideoTexture::textureId() const
{
    return m_textureId;
}

QSize GStreamerVideoTexture::textureSize() const
{
    return m_textureSize;
}

void GStreamerVideoTexture::setTextureSize(const QSize &size)
{
    m_textureSize = size;
}

bool GStreamerVideoTexture::hasAlphaChannel() const
{
    return false;
}

bool GStreamerVideoTexture::hasMipmaps() const
{
    return false;
}

QRectF GStreamerVideoTexture::normalizedTextureSubRect() const
{
    return m_subRect;
}

void GStreamerVideoTexture::bind()
{
    if (m_textureId) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_textureId);
    }
}

bool GStreamerVideoTexture::updateTexture()
{
    static const PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES
            = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));

    if (m_updated) {
        return true;
    }

    NemoGstVideoTexture *sink = NEMO_GST_VIDEO_TEXTURE(m_sink);

    if (!nemo_gst_video_texture_acquire_frame(sink)) {
        return false;
    }

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    static const GQuark cropQuark = g_quark_from_string("GstDroidCamSrcCropData");
    if (const GstStructure *crop = nemo_gst_video_texture_get_frame_qdata(
                sink, cropQuark)) {
        gst_structure_get_int(crop, "left", &left);
        gst_structure_get_int(crop, "top", &top);
        gst_structure_get_int(crop, "right", &right);
        gst_structure_get_int(crop, "bottom", &bottom);
    }

    if (left != right && top != bottom) {
        m_subRect = QRectF(
                    qreal(left) / m_textureSize.width(),
                    qreal(top) / m_textureSize.height(),
                    qreal(right - left) / m_textureSize.width(),
                    qreal(bottom - top) / m_textureSize.height());
    } else {
        m_subRect = QRectF(0, 0, 1, 1);
    }

    EGLImageKHR image;
    if (!nemo_gst_video_texture_bind_frame(sink, &image)) {

        invalidateTexture();

        nemo_gst_video_texture_release_frame(sink, NULL);
        return false;
    } else {
        if (!m_textureId) {
            glGenTextures(1, &m_textureId);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_textureId);
            glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameterf(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_textureId);
        }
        glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
        m_updated = true;
        return true;
    }
}

void GStreamerVideoTexture::invalidateTexture()
{
    if (m_textureId) {
        glDeleteTextures(1, &m_textureId);
        m_textureId = 0;
    }
}

void GStreamerVideoTexture::releaseTexture()
{
    static const PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR
            = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));

    if (m_updated) {
        m_updated = false;

        NemoGstVideoTexture *sink = NEMO_GST_VIDEO_TEXTURE(m_sink);

        nemo_gst_video_texture_unbind_frame(sink);

        invalidateTexture();

        EGLSyncKHR sync = eglCreateSyncKHR(m_display, EGL_SYNC_FENCE_KHR, NULL);
        nemo_gst_video_texture_release_frame(sink, sync);
    }
}

class GStreamerVideoMaterialShader : public QSGMaterialShader
{
public:
    static QSGMaterialType type;

    void updateState(const RenderState &state, QSGMaterial *newEffect, QSGMaterial *oldEffect);
    char const *const *attributeNames() const;

protected:
    void initialize();

    const char *vertexShader() const;
    const char *fragmentShader() const;

private:
    int id_matrix;
    int id_subrect;
    int id_opacity;
    int id_texture;
};

void GStreamerVideoMaterialShader::updateState(
        const RenderState &state, QSGMaterial *newEffect, QSGMaterial *oldEffect)
{
    GStreamerVideoMaterial *material = static_cast<GStreamerVideoMaterial *>(newEffect);

    if (state.isMatrixDirty()) {
        program()->setUniformValue(id_matrix, state.combinedMatrix());
    }

    if (state.isOpacityDirty()) {
        program()->setUniformValue(id_opacity, state.opacity());
    }

    if (!oldEffect) {
        program()->setUniformValue(id_texture, 0);
    }

    const QRectF subRect = material->m_texture->normalizedTextureSubRect();
    program()->setUniformValue(
                id_subrect, QVector4D(subRect.x(), subRect.y(), subRect.width(), subRect.height()));

    glActiveTexture(GL_TEXTURE0);
    material->m_texture->bind();
}

char const *const *GStreamerVideoMaterialShader::attributeNames() const
{
    static char const *const attr[] = { "position", "texcoord", 0 };
    return attr;
}

void GStreamerVideoMaterialShader::initialize()
{
    id_matrix = program()->uniformLocation("matrix");
    id_subrect = program()->uniformLocation("subrect");
    id_opacity = program()->uniformLocation("opacity");
    id_texture = program()->uniformLocation("texture");
}

QSGMaterialType GStreamerVideoMaterialShader::type;

const char *GStreamerVideoMaterialShader::vertexShader() const
{
    return  "\n uniform highp mat4 matrix;"
            "\n uniform highp vec4 subrect;"
            "\n attribute highp vec4 position;"
            "\n attribute highp vec2 texcoord;"
            "\n varying highp vec2 frag_tx;"
            "\n void main(void)"
            "\n {"
            "\n     gl_Position = matrix * position;"
            "\n     frag_tx = (texcoord * subrect.zw) + subrect.xy;"
            "\n }";
}

const char *GStreamerVideoMaterialShader::fragmentShader() const
{
    return  "\n #extension GL_OES_EGL_image_external : require"
            "\n uniform samplerExternalOES texture;"
            "\n uniform lowp float opacity;\n"
            "\n varying highp vec2 frag_tx;"
            "\n void main(void)"
            "\n {"
            "\n     gl_FragColor = opacity * texture2D(texture, frag_tx.st);"
            "\n }";
}

GStreamerVideoMaterial::GStreamerVideoMaterial(GStreamerVideoTexture *texture)
    : m_texture(texture)
{
}

QSGMaterialShader *GStreamerVideoMaterial::createShader() const
{
    return new GStreamerVideoMaterialShader;
}

QSGMaterialType *GStreamerVideoMaterial::type() const
{
    return &GStreamerVideoMaterialShader::type;
}

int GStreamerVideoMaterial::compare(const QSGMaterial *other) const
{
    return m_texture - static_cast<const GStreamerVideoMaterial *>(other)->m_texture;
}

GStreamerVideoNode::GStreamerVideoNode(GStreamerVideoTexture *texture)
    : m_material(texture)
    , m_geometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4)
{
    setGeometry(&m_geometry);
    setMaterial(&m_material);
    setFlag(UsePreprocess);
}

GStreamerVideoNode::~GStreamerVideoNode()
{
}

void GStreamerVideoNode::preprocess()
{
    GStreamerVideoTexture *t = m_material.m_texture;
    if (t && t->updateTexture())
        markDirty(QSGNode::DirtyMaterial);
}

void GStreamerVideoNode::setBoundingRect(
        const QRectF &rect, int orientation, bool horizontalMirror, bool verticalMirror)
{
    // Texture vertices clock wise from top left: or tl, tr, br, lf
    // Vertex order is tl, bl, tr, br. So unrotated the texture indexes are [0, 3, 1, 2] and
    // by shifting the array left and wrapping we rotate the image in 90 degree increments.
    const float tx[] = { 0, 1, 1, 0 };
    const float ty[] = { 0, 0, 1, 1 };

    // Texture coordinates are 0, or 1 so flip by subtracting the cooridinate from 1 and
    // taking the absolute value. 1 - 0 = 1, 1 - 1 = 0.  The absolute of 0 take the coordinate
    // gives back the original value. 0 - 0 = 0, 0 - 1 = -1.
    const float hm = horizontalMirror ? 1 : 0;
    const float vm = verticalMirror ? 1 : 0;

    const int offset = orientation / 90;
    QSGGeometry::TexturedPoint2D vertices[] = {
        { rect.left() , rect.top()   , qAbs(hm - tx[(0 + offset) % 4]), qAbs(vm - ty[(0 + offset) % 4]) },
        { rect.left() , rect.bottom(), qAbs(hm - tx[(3 + offset) % 4]), qAbs(vm - ty[(3 + offset) % 4]) },
        { rect.right(), rect.top()   , qAbs(hm - tx[(1 + offset) % 4]), qAbs(vm - ty[(1 + offset) % 4]) },
        { rect.right(), rect.bottom(), qAbs(hm - tx[(2 + offset) % 4]), qAbs(vm - ty[(2 + offset) % 4]) }
    };

    memcpy(m_geometry.vertexDataAsTexturedPoint2D(), vertices, sizeof(vertices));
}

class ImplicitSizeVideoOutput : public QDeclarativeVideoOutput
{
public:
    using QQuickItem::setImplicitSize;
};

class NemoVideoTextureBackend : public QObject, public QDeclarativeVideoBackend
{
    Q_OBJECT
public:
    explicit NemoVideoTextureBackend(QDeclarativeVideoOutput *parent);
    virtual ~NemoVideoTextureBackend();

    bool init(QMediaService *service);
    void releaseSource();
    void releaseControl();
    void itemChange(QQuickItem::ItemChange change, const QQuickItem::ItemChangeData &changeData);
    QSize nativeSize() const;
    void updateGeometry();
    QSGNode *updatePaintNode(QSGNode *oldNode, QQuickItem::UpdatePaintNodeData *data);
    QAbstractVideoSurface *videoSurface() const;

    // The viewport, adjusted for the pixel aspect ratio
    QRectF adjustedViewport() const;

    bool event(QEvent *event);

signals:
    void nativeSizeChanged();

private slots:
    void orientationChanged();
    void mirrorChanged();

private:
    static void frame_ready(GstElement *sink, int frame, void *data);

    QMutex m_mutex;
    QPointer<QGStreamerElementControl> m_control;
    GstElement *m_sink;
    EGLDisplay m_display;
    GStreamerVideoTexture *m_texture;
    QSize m_nativeSize;
    QSize m_textureSize;
    QSize m_implicitSize;
    gulong m_signalId;
    int m_orientation;
    int m_textureOrientation;
    bool m_mirror;
    bool m_active;
    bool m_geometryChanged;
    bool m_frameChanged;
};

NemoVideoTextureBackend::NemoVideoTextureBackend(QDeclarativeVideoOutput *parent)
    : QDeclarativeVideoBackend(parent)
    , m_sink(0)
    , m_display(0)
    , m_texture(0)
    , m_signalId(0)
    , m_orientation(0)
    , m_textureOrientation(0)
    , m_mirror(false)
    , m_active(false)
    , m_geometryChanged(false)
    , m_frameChanged(false)
{
    if (QPlatformNativeInterface *nativeInterface = QGuiApplication::platformNativeInterface()) {
        m_display = nativeInterface->nativeResourceForIntegration("egldisplay");
    }
    if (!m_display) {
        m_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }

    if ((m_sink = gst_element_factory_make("droideglsink", NULL))) {
        // Take ownership of the element or it will be destroyed when any bin it was added to is.
        gst_object_ref(GST_OBJECT(m_sink));
        gst_object_sink(GST_OBJECT(m_sink));

        m_signalId = g_signal_connect(
                    G_OBJECT(m_sink), "frame-ready", G_CALLBACK(frame_ready), this);
    }
}

NemoVideoTextureBackend::~NemoVideoTextureBackend()
{
    releaseControl();

    if (m_sink) {
        QMutexLocker locker(&m_mutex);

        g_signal_handler_disconnect(G_OBJECT(m_sink), m_signalId);
        gst_object_unref(GST_OBJECT(m_sink));
        m_sink = 0;
    }

    if (m_texture) {
        m_texture->deleteLater();
    }
}

void NemoVideoTextureBackend::orientationChanged()
{
    const int orientation = q->orientation();
    if (m_orientation != orientation) {
        m_orientation = orientation;
        m_geometryChanged = true;
        q->update();
    }
}

void NemoVideoTextureBackend::mirrorChanged()
{
    const bool mirror = q->property("mirror").toBool();
    if (m_mirror != mirror) {
        m_mirror = mirror;
        m_geometryChanged = true;
        q->update();
    }
}

bool NemoVideoTextureBackend::init(QMediaService *service)
{
    if (!m_sink) {
        return false;
    }

    QMediaControl *control = service->requestControl(QGStreamerVideoSinkControl_iid);
    if (control) {
        m_control = qobject_cast<QGStreamerElementControl *>(control);
        if (!m_control) {
            service->releaseControl(control);
            return false;
        }
    } else {
        return false;
    }

    m_service = service;
    m_control->setElement(m_sink);

    connect(this, SIGNAL(nativeSizeChanged()), q, SLOT(_q_updateNativeSize()));
    connect(q, SIGNAL(orientationChanged()), this, SLOT(orientationChanged()));

    const int mirrorIndex = q->metaObject()->indexOfProperty("mirror");
    if (mirrorIndex != -1) {
        QMetaObject::connect(
                    q, q->metaObject()->property(mirrorIndex).notifySignalIndex(),
                    this, staticMetaObject.indexOfMethod("mirrorChanged()"));
        mirrorChanged();
    }

    return true;
}

void NemoVideoTextureBackend::releaseSource()
{
}

void NemoVideoTextureBackend::releaseControl()
{
    if (m_service && m_control) {
        m_service->releaseControl(m_control.data());
        m_control = 0;
    }
}

void NemoVideoTextureBackend::itemChange(QQuickItem::ItemChange, const QQuickItem::ItemChangeData &)
{
}

QSize NemoVideoTextureBackend::nativeSize() const
{
    return m_nativeSize;
}

void NemoVideoTextureBackend::updateGeometry()
{
    m_geometryChanged = true;
}

QSGNode *NemoVideoTextureBackend::updatePaintNode(QSGNode *oldNode, QQuickItem::UpdatePaintNodeData *)
{
    GStreamerVideoNode *node = static_cast<GStreamerVideoNode *>(oldNode);

    if (!m_active) {
        if (m_texture) {
            m_texture->invalidateTexture();
        }

        delete node;

        NemoGstVideoTexture *sink = NEMO_GST_VIDEO_TEXTURE(m_sink);
        nemo_gst_video_texture_detach_from_display(sink);

        return 0;
    }

    if (!m_texture) {
        m_texture = new GStreamerVideoTexture(m_sink, m_display);
        connect(q->window(), SIGNAL(afterRendering()),
                m_texture, SLOT(releaseTexture()),
                Qt::DirectConnection);
    }
    m_texture->setTextureSize(m_textureSize);

    if (!node) {
        NemoGstVideoTexture *sink = NEMO_GST_VIDEO_TEXTURE(m_sink);
        nemo_gst_video_texture_attach_to_display(sink, m_display);

        node = new GStreamerVideoNode(m_texture);
        m_geometryChanged = true;
    }

    if (m_geometryChanged) {
        const QRectF br = q->boundingRect();

        QRectF rect(QPointF(0, 0), QSizeF(m_nativeSize).scaled(br.size(), Qt::KeepAspectRatio));
        rect.moveCenter(br.center());

        int orientation = (m_orientation - m_textureOrientation) % 360;
        if (orientation < 0)
            orientation += 360;

        node->setBoundingRect(
                    rect,
                    orientation,
                    m_mirror && (m_textureOrientation % 180) == 0,
                    m_mirror && (m_textureOrientation % 180) != 0);
        node->markDirty(QSGNode::DirtyGeometry);
        m_geometryChanged = false;
    }

    if (m_frameChanged) {
        node->markDirty(QSGNode::DirtyMaterial);
        m_frameChanged = false;
    }

    return node;
}

QAbstractVideoSurface *NemoVideoTextureBackend::videoSurface() const
{
    return 0;
}

// The viewport, adjusted for the pixel aspect ratio
QRectF NemoVideoTextureBackend::adjustedViewport() const
{
    const QRectF br = q->boundingRect();

    QRectF rect(QPointF(0, 0), QSizeF(m_nativeSize).scaled(br.size(), Qt::KeepAspectRatio));
    rect.moveCenter(br.center());

    return rect;
}

bool NemoVideoTextureBackend::event(QEvent *event)
{
    if (event->type() == QEvent::Resize) {
        m_nativeSize = static_cast<QResizeEvent *>(event)->size();
        if (m_nativeSize.isValid()) {
            if ((m_orientation % 180) != 0) {
                m_nativeSize.transpose();
            }

            static_cast<ImplicitSizeVideoOutput *>(q)->setImplicitSize(
                        m_nativeSize.width(), m_nativeSize.height());
        }
        q->update();
        emit nativeSizeChanged();
        return true;
    } else if (event->type() == QEvent::UpdateRequest) {
        q->update();
        return true;
    } else {
        return QObject::event(event);
    }
}

void NemoVideoTextureBackend::frame_ready(GstElement *, int frame, void *data)
{
    NemoVideoTextureBackend *instance = static_cast<NemoVideoTextureBackend *>(data);

    QSize implicitSize;

    if (frame < 0) {
        instance->m_active = false;
        QCoreApplication::postEvent(instance, new QEvent(QEvent::UpdateRequest));
    } else {
        QMutexLocker locker(&instance->m_mutex);

        if (!instance->m_sink) {
            return;
        }

        if (GstCaps *caps = gst_pad_get_negotiated_caps(
                       gst_element_get_static_pad(instance->m_sink, "sink"))) {
            QSize textureSize;

            const GstStructure *structure = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(structure, "width", &textureSize.rwidth());
            gst_structure_get_int(structure, "height", &textureSize.rheight());

            implicitSize = textureSize;
            gint numerator = 0;
            gint denominator = 0;
            if (gst_structure_get_fraction(structure, "pixel-aspect-ratio", &numerator, &denominator)
                    && denominator > 0) {
                implicitSize.setWidth(implicitSize.width() * numerator / denominator);
            }

            int orientation = 0;
            gst_structure_get_int(structure, "orientation-angle", &orientation);

            instance->m_textureOrientation = orientation >= 0 ? orientation : 0;
            instance->m_textureSize = textureSize;
            instance->m_geometryChanged = true;
            instance->m_active = true;

            if (orientation % 180 != 0) {
                implicitSize.transpose();
            }

            gst_caps_unref(caps);
        }

        instance->m_frameChanged = true;

        if (instance->m_implicitSize != implicitSize) {
            instance->m_implicitSize = implicitSize;

            QCoreApplication::postEvent(instance, new QResizeEvent(implicitSize, implicitSize));
        } else {
            QCoreApplication::postEvent(instance, new QEvent(QEvent::UpdateRequest));
        }
    }
}

class NemoVideoTextureBackendPlugin : public QObject, public QDeclarativeVideoBackendFactoryInterface
{
    Q_OBJECT
    Q_INTERFACES(QDeclarativeVideoBackendFactoryInterface)
    Q_PLUGIN_METADATA(IID "org.qt-project.qt.declarativevideobackendfactory/5.2" FILE "videotexturebackend.json")
public:
    NemoVideoTextureBackendPlugin();

    QDeclarativeVideoBackend *create(QDeclarativeVideoOutput *parent);
};

NemoVideoTextureBackendPlugin::NemoVideoTextureBackendPlugin()
{
    gst_init(0, 0);
}

QDeclarativeVideoBackend *NemoVideoTextureBackendPlugin::create(QDeclarativeVideoOutput *parent)
{
    return new NemoVideoTextureBackend(parent);
}

#include "videotexturebackend.moc"
