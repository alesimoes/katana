#!/bin/sh
# Monta o AppImage do katana (motor + aplicativo Qt, com as bibliotecas do Qt dentro).
# Uso: desktop-qt/packaging/appimage.sh   ->  desktop-qt/packaging/out/katana-x86_64.AppImage
# O AppImage roda em sistemas com glibc igual ou mais nova que a da maquina onde foi montado:
# para distribuir, monte numa base antiga (o fluxo .github/workflows/pacotes.yml faz isso).
# Ferramentas (linuxdeploy e o plugin do Qt) sao baixadas das paginas oficiais para packaging/tools/.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
tools="$here/tools"; out="$here/out"; app="$here/AppDir"
mkdir -p "$tools" "$out"

for u in linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
         linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage; do
    f="$tools/$(basename "$u")"
    [ -s "$f" ] || curl -sSL -o "$f" "https://github.com/$u"
    chmod +x "$f"
done

# motor e aplicativo
[ -x "$root/katana" ] || (cd "$root" && ./build.sh)
[ -x "$root/desktop-qt/build/katana-desktop-qt" ] || (cd "$root/desktop-qt" && ./build.sh)

rm -rf "$app"
mkdir -p "$app/usr/bin" "$app/usr/share/applications" "$app/usr/share/icons/hicolor/256x256/apps"
cp "$root/desktop-qt/build/katana-desktop-qt" "$app/usr/bin/katana-desktop"
cp "$root/katana" "$app/usr/bin/katana"
cp "$root/desktop-qt/icons/katana-256.png" "$app/usr/share/icons/hicolor/256x256/apps/katana.png"
cat > "$app/usr/share/applications/katana.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Katana
GenericName=Agente de IA local
Comment=Agente de IA que roda no seu computador
Exec=katana-desktop
Icon=katana
Terminal=false
Categories=Utility;
StartupWMClass=katana-desktop
EOF

# as ferramentas sao AppImages: sem FUSE (conteineres, CI) rodam extraidas
export APPIMAGE_EXTRACT_AND_RUN=1
export QML_SOURCES_PATHS="$root/desktop-qt/qml"
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so;libqoffscreen.so"
export EXTRA_QT_PLUGINS="waylandcompositor"
export OUTPUT="$out/katana-x86_64.AppImage"
export NO_STRIP=1       # o strip embutido na ferramenta nao conhece as secoes dos binarios novos
cd "$here"
"$tools/linuxdeploy-x86_64.AppImage" --appdir "$app" --plugin qt --output appimage \
    --desktop-file "$app/usr/share/applications/katana.desktop" \
    --icon-file "$app/usr/share/icons/hicolor/256x256/apps/katana.png" \
    --executable "$app/usr/bin/katana-desktop" --executable "$app/usr/bin/katana"
ls -la "$OUTPUT"
