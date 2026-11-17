#!/bin/sh
# Instala o katana-desktop no menu de aplicativos e fixa na dock (GNOME/Ubuntu).
# Uso: desktop-qt/instalar-dock.sh
# - copia katana-desktop.desktop para ~/.local/share/applications/ (Icon aponta para o PNG instalado);
# - copia os icones para ~/.local/share/icons/hicolor/<tam>/apps/katana-desktop.png (e o SVG em scalable);
# - atualiza os caches (update-desktop-database, gtk-update-icon-cache) se existirem;
# - acrescenta 'katana-desktop.desktop' a org.gnome.shell favorite-apps sem remover nem duplicar.
# Nao mexe no binario: o Exec do .desktop usa o link /mnt/dados/source/katana/katana-desktop.
set -e
cd "$(dirname "$0")"
APPS="$HOME/.local/share/applications"
ICONS="$HOME/.local/share/icons/hicolor"
DESK=katana-desktop.desktop

echo "ambiente: ${XDG_CURRENT_DESKTOP:-desconhecido}"

mkdir -p "$APPS"
for t in 16 32 48 64 128 256 512; do
    mkdir -p "$ICONS/${t}x${t}/apps"
    cp "icons/katana-$t.png" "$ICONS/${t}x${t}/apps/katana-desktop.png"
done
mkdir -p "$ICONS/scalable/apps"
cp icons/katana.svg "$ICONS/scalable/apps/katana-desktop.svg"

# copia instalada: icone com caminho absoluto do PNG ja instalado
sed "s|^Icon=.*|Icon=$ICONS/256x256/apps/katana-desktop.png|" "$DESK" > "$APPS/$DESK"
chmod 644 "$APPS/$DESK"
echo "instalado: $APPS/$DESK"

command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS" || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -f -t "$ICONS" >/dev/null 2>&1 || true

case "${XDG_CURRENT_DESKTOP:-}" in
    *GNOME*|*Unity*|*ubuntu*)
        command -v gsettings >/dev/null 2>&1 || { echo "gsettings ausente: fixe na dock manualmente"; exit 0; }
        atual=$(gsettings get org.gnome.shell favorite-apps)
        case "$atual" in
            *"'$DESK'"*) echo "ja estava na dock: $atual" ;;
            *)
                case "$atual" in
                    "@as []"|"[]") novo="['$DESK']" ;;
                    *) novo="${atual%]}, '$DESK']" ;;
                esac
                gsettings set org.gnome.shell favorite-apps "$novo"
                echo "dock antes:  $atual"
                echo "dock agora:  $(gsettings get org.gnome.shell favorite-apps)"
                ;;
        esac
        ;;
    *)
        echo "ambiente sem suporte automatico a dock; o atalho foi instalado no menu de aplicativos."
        ;;
esac
