import QtQuick

// A Lucide icon tinted to `color`, rendered by the "icon" image provider.
Image {
    id: root
    property string name: "search"
    property color color: "white"
    property int size: 18
    width: size
    height: size
    sourceSize: Qt.size(size * 2, size * 2)
    source: name ? "image://icon/" + name + "?color=" + encodeURIComponent(color.toString()) : ""
    fillMode: Image.PreserveAspectFit
    smooth: true
    cache: true
}
