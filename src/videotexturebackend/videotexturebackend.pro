TEMPLATE = lib
TARGET = gstnemovideotexturebackend
TARGET = $$qtLibraryTarget($$TARGET)

QT += \
        gui-private \
        quick \
        multimedia \
        multimedia-private \
        qtmultimediaquicktools-private

CONFIG += plugin hide_symbols link_pkgconfig

PKGCONFIG +=\
        egl \
        gstreamer-0.10 \
        nemo-gstreamer-interfaces-0.10

LIBS += -lqgsttools_p

# It won't compile without this,
# the issue is Xlib.h defines Bool as int but  QJsonValue.h has an enum with Bool = 0x1 --> int = 0x1 -> BOOM!
DEFINES += MESA_EGL_NO_X11_HEADERS

SOURCES += \
        videotexturebackend.cpp

target.path = $$[QT_INSTALL_PLUGINS]/video/declarativevideobackend

INSTALLS += target
