(() => {
  "use strict";

  const isTextField = element => element && element.matches("input[type=text],input[type=tel],textarea");
  let requestedField = null;
  let editingField = null;
  let tabNavigation = false;

  function dismissKeyboard() {
    requestedField = editingField = null;
    if (isTextField(document.activeElement)) document.activeElement.blur();
  }

  function prepareEdit(event) {
    const label = event.target.closest("label");
    const field = isTextField(event.target) ? event.target : label && label.control;
    requestedField = isTextField(field) && document.activeElement !== field ? field : null;
    if (!isTextField(field)) dismissKeyboard();
  }

  // iPhone Wi-Fi portals can restore the first input's focus after keyboard
  // dismissal or preview updates. Only a deliberate field/label activation,
  // keyboard navigation or form validation may begin a new editing session.
  document.addEventListener("pointerdown", prepareEdit, true);
  document.addEventListener("click", prepareEdit, true);
  document.addEventListener("focusin", event => {
    const field = event.target;
    const allowed = field === requestedField || field === editingField || tabNavigation;
    requestedField = null;
    tabNavigation = false;
    if (!isTextField(field)) return;
    if (allowed) editingField = field;
    else field.blur();
  });
  document.addEventListener("focusout", event => {
    if (event.target === editingField) editingField = null;
    if (event.target === requestedField) requestedField = null;
  });
  document.addEventListener("invalid", event => {
    if (isTextField(event.target)) requestedField = event.target;
  }, true);

  // Captive-portal windows can restore focus when the page opens or returns.
  // Do this once per page show; never repeatedly steal focus while typing.
  dismissKeyboard();
  window.addEventListener("pageshow", dismissKeyboard);
  document.addEventListener("keydown", event => {
    if (event.key === "Tab") {
      tabNavigation = true;
      setTimeout(() => { tabNavigation = false; }, 0);
    }
    if (isTextField(event.target) && !event.isComposing &&
        (event.key === "Enter" || event.key === "Escape")) {
      event.preventDefault();
      dismissKeyboard();
    }
  });
})();
