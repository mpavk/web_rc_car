#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &> /dev/null && pwd)

cleanup() {
  echo
  echo "Завершення роботи... Зупинка фонового процесу камери."
  if [ -n "${CAMERA_PID-}" ] && ps -p $CAMERA_PID > /dev/null; then
    echo "  → Завершуємо процес камери (PID $CAMERA_PID)"
    kill $CAMERA_PID || true
  fi
  echo "Всі процеси зупинено."
}
trap cleanup EXIT

echo "Запуск скрипта камери у фоні..."
"$SCRIPT_DIR/start_camera.sh" &
CAMERA_PID=$!
echo "  → CAMERA_PID=$CAMERA_PID"


sleep 1
if ps -p $CAMERA_PID > /dev/null; then
  echo "  ✓ Камера успішно запущена (PID $CAMERA_PID)."
else
  echo "  ✗ Помилка запуску камери!" >&2
  exit 1
fi

"$SCRIPT_DIR/build/webrccar" ip port vid

