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

SOURCES += \
        videotexturebackend.cpp

target.path = $$[QT_INSTALL_PLUGINS]/video/declarativevideobackend

INSTALLS += target
