(() => {
  "use strict";

  const toggle = document.getElementById("view-positioning");
  const preview = document.getElementById("positioning-preview");
  const image = document.getElementById("positioning-image");
  const status = document.getElementById("positioning-status");
  let timer = null;
  let request = null;
  let imageUrl = null;
  let leaving = false;

  const viewing = () => toggle.checked && !document.hidden && !leaving;

  function clearImage() {
    image.hidden = true;
    image.removeAttribute("src");
    if (imageUrl) URL.revokeObjectURL(imageUrl);
    imageUrl = null;
  }

  function stop() {
    clearTimeout(timer);
    if (request) request.abort();
    request = null;
    clearImage();
  }

  async function refresh() {
    if (!viewing()) return;
    const started = Date.now();
    const controller = new AbortController();
    request = controller;
    timer = setTimeout(() => controller.abort(), 2500);
    try {
      const response = await fetch("/positioning.jpg?t=" + started, {
        cache: "no-store", signal: controller.signal
      });
      if (!response.ok) throw new Error("Camera not ready");
      const frame = await response.blob();
      if (!viewing() || request !== controller) return;
      const previousUrl = imageUrl;
      imageUrl = URL.createObjectURL(frame);
      image.src = imageUrl;
      image.hidden = false;
      if (previousUrl) URL.revokeObjectURL(previousUrl);
      status.textContent = "Live preview · 1 FPS";
    } catch (error) {
      if (viewing() && request === controller) {
        clearImage();
        status.textContent = "Camera unavailable. Retrying...";
      }
    } finally {
      if (request === controller) {
        clearTimeout(timer);
        request = null;
        if (viewing()) timer = setTimeout(refresh, Math.max(0, 1000 - (Date.now() - started)));
      }
    }
  }

  function update() {
    stop();
    preview.hidden = !toggle.checked;
    if (viewing()) {
      status.textContent = "Waiting for camera...";
      refresh();
    }
  }

  toggle.checked = false;
  preview.hidden = true;
  toggle.addEventListener("change", update);
  document.addEventListener("visibilitychange", update);
  document.querySelector("form").addEventListener("submit", () => {
    leaving = true;
    stop();
  });
  window.addEventListener("pagehide", () => {
    leaving = true;
    stop();
  });
  window.addEventListener("pageshow", () => {
    leaving = false;
    update();
  });
})();
