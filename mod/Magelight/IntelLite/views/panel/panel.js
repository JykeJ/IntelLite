/* IntelLite panel page.
 *
 * JS <-> C++ bridge:
 *   page -> plugin : window.magelight.send(name, arg)
 *                    ('panelReady' | 'panelClose'
 *                     | 'requestTasks' | 'cancelTask' | 'executeAction'
 *                     | 'searchActors' | 'setConfirm'
 *                     | 'blacklistAdd' | 'blacklistRemove')
 *   plugin -> page : window.intelite.setStatus(obj)
 *                    window.intelite.setNearby(array of {name, distance, formId})
 *                    window.intelite.setTasks(array of {id, type, agent, target,
 *                                            state, detail, canCancel})
 *                    window.intelite.setSearchResults(array of {name, formId, editorid})
 *                    window.intelite.setExecutorSearchResults(array of {name, formId, editorid})
 */
(function () {
  'use strict';

  function el(id) { return document.getElementById(id); }

  function send(name, arg) {
    if (window.magelight && typeof window.magelight.send === 'function') {
      window.magelight.send(name, arg === undefined ? '' : arg);
    }
  }

  // Custom in-page dropdowns. The renderer draws a native <select> popup
  // itself, which lets two of them overlap and ignores the page font; these
  // render the menu inside the page instead. Only one is ever open, and the
  // menu is positioned and sized to the control.
  var dropdowns = [];

  function closeDropdowns(except) {
    for (var i = 0; i < dropdowns.length; i++) {
      if (dropdowns[i] !== except) dropdowns[i].close();
    }
  }

  function makeDropdown(mountId, items, initial, onChange) {
    var mount = el(mountId);
    if (!mount) return null;
    var value = initial;
    var open = false;

    var toggle = document.createElement('button');
    toggle.type = 'button';
    toggle.className = 'dd-toggle';
    var label = document.createElement('span');
    label.className = 'dd-label';
    var caret = document.createElement('span');
    caret.className = 'dd-caret';
    caret.textContent = '\u25BE';
    toggle.appendChild(label);
    toggle.appendChild(caret);

    var menu = document.createElement('ul');
    menu.className = 'dd-menu';
    var itemEls = [];

    function labelOf(v) {
      for (var i = 0; i < items.length; i++) {
        if (items[i].value === v) return items[i].label;
      }
      return '';
    }

    function render() {
      label.textContent = labelOf(value);
      for (var i = 0; i < itemEls.length; i++) {
        itemEls[i].classList.toggle('selected',
          itemEls[i].getAttribute('data-value') === value);
      }
      mount.classList.toggle('open', open);
    }

    for (var i = 0; i < items.length; i++) {
      (function (it) {
        var li = document.createElement('li');
        li.className = 'dd-item';
        li.setAttribute('data-value', it.value);
        li.textContent = it.label;
        li.addEventListener('click', function (ev) {
          ev.stopPropagation();
          var changed = (value !== it.value);
          value = it.value;
          open = false;
          render();
          if (changed && onChange) onChange(value);
        });
        menu.appendChild(li);
        itemEls.push(li);
      })(items[i]);
    }

    toggle.addEventListener('click', function (ev) {
      ev.stopPropagation();
      open = !open;
      if (open) closeDropdowns(api);
      render();
    });

    mount.appendChild(toggle);
    mount.appendChild(menu);
    render();

    var api = {
      get value() { return value; },
      close: function () { if (open) { open = false; render(); } }
    };
    dropdowns.push(api);
    return api;
  }

  var nearby = [];            // [{ name, distance, formId }]
  var execSearchResults = []; // [{ name, formId, editorid }]
  var searchResults = [];     // [{ name, formId, editorid }]
  var tasks = [];             // [{ id, type, agent, target, state, detail, canCancel }]
  var blacklist = [];         // [editorid]
  var selectedAgent = null;   // formId
  var selectedTarget = null;  // formId

  var STATE_LABELS = {
    pending: 'Pending',
    traveling: 'Traveling',
    returning: 'Returning',
    executing: 'Executing',
    lingering: 'Lingering',
    done: 'Done',
    cancelled: 'Cancelled',
    failed: 'Failed'
  };

  var TYPE_LABELS = {
    fetch: 'Fetch',
    deliver: 'Deliver',
    meet: 'Meet',
    assassinate: 'Assassinate'
  };

  function nameOf(formId, list) {
    if (formId == null) return '';
    for (var i = 0; i < list.length; i++) {
      if (list[i].formId === formId) return list[i].name || 'Unknown';
    }
    return '';
  }

  function hasFormId(list, formId) {
    if (formId == null) return false;
    for (var i = 0; i < list.length; i++) {
      if (list[i].formId === formId) return true;
    }
    return false;
  }

  // The assassinate fee: an empty or non-positive field means "no fee" (0).
  function parseFee() {
    var raw = el('fee-input').value;
    var fee = raw ? parseInt(raw, 10) : 0;
    return (isNaN(fee) || fee < 0) ? 0 : fee;
  }

  function updateSummary() {
    var action = actionDd.value;
    var agent = nameOf(selectedAgent, nearby) || nameOf(selectedAgent, execSearchResults);
    if (action === 'meet') {
      var place = (meetModeDd.value === 'Named')
        ? (el('meet-location').value.trim() || '-')
        : 'Here';
      var hours = el('meet-hours').value || '2';
      el('exec-summary').textContent =
        'Executor: ' + (agent || '-') + '  Meeting: ' + place + ' in ' + hours + 'h';
      return;
    }
    // A target can be picked from the nearby list or from search results.
    var target = nameOf(selectedTarget, nearby) || nameOf(selectedTarget, searchResults);
    if (action === 'assassinate') {
      var fee = parseFee();
      el('exec-summary').textContent =
        'Executor: ' + (agent || '-') + '  Target: ' + (target || '-') +
        '  Fee: ' + (fee > 0 ? fee + 'g' : 'none');
      return;
    }
    el('exec-summary').textContent =
      'Executor: ' + (agent || '-') + '  Target: ' + (target || '-');
  }

  // Show only the parameter controls that belong to the selected action.
  function updateActionParams() {
    var action = actionDd.value;
    el('target-params').classList.toggle('active',
      action === 'fetch' || action === 'deliver' || action === 'assassinate');
    el('deliver-params').classList.toggle('active', action === 'deliver');
    el('meet-params').classList.toggle('active', action === 'meet');
    el('fee-params').classList.toggle('active', action === 'assassinate');
    updateMeetLocationVisibility();
  }

  // The named-place text input is shown only for "Named" while Meet is active.
  function updateMeetLocationVisibility() {
    var named = meetModeDd.value === 'Named';
    el('meet-named-row').style.display =
      (actionDd.value === 'meet' && named) ? '' : 'none';
    updateSummary();
  }

  function renderStatus(status) {
    var s = status || {};
    el('set-mod-version').textContent = s.modVersion != null ? s.modVersion : '-';

    var api = s.skyrimnetApi;
    el('set-skyrimnet-api').textContent =
      (api == null) ? '-' : (api < 0 ? 'not present' : String(api));

    el('set-magelight-host').textContent = s.magelightHost || 'not present';
    el('set-hotkey').textContent = s.hotkey || '-';
    var hotkeyHint = el('hotkey-hint');
    if (hotkeyHint) hotkeyHint.textContent = s.hotkeyError || '';

    // The confirmation preference (0/1). Reflected on the checkbox without
    // echoing a change back to the plugin.
    el('confirm-toggle').checked = (s.confirm === 1 || s.confirm === true);

    renderBlacklist(s.blacklist);
  }

  // The Settings tab's blacklist: one row per editorid with a Remove button.
  function drawBlacklist() {
    var ul = el('blacklist-list');
    ul.textContent = '';

    if (blacklist.length === 0) {
      var empty = document.createElement('li');
      empty.className = 'empty';
      empty.textContent = 'No blacklisted names';
      ul.appendChild(empty);
      return;
    }

    for (var i = 0; i < blacklist.length; i++) {
      (function (editorid) {
        var li = document.createElement('li');
        li.className = 'npc';

        var name = document.createElement('span');
        name.className = 'npc-name';
        name.textContent = editorid;
        li.appendChild(name);

        var btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'remove-btn';
        btn.textContent = 'Remove';
        btn.addEventListener('click', function (ev) {
          ev.stopPropagation();
          send('blacklistRemove', editorid);
        });
        li.appendChild(btn);
        ul.appendChild(li);
      })(blacklist[i]);
    }
  }

  function renderBlacklist(list) {
    blacklist = Array.isArray(list) ? list : [];
    drawBlacklist();
  }

  function drawNearby() {
    var ul = el('nearby-list');
    ul.textContent = '';

    if (nearby.length === 0) {
      var empty = document.createElement('li');
      empty.className = 'empty';
      empty.textContent = 'No NPCs nearby';
      ul.appendChild(empty);
      return;
    }

    for (var i = 0; i < nearby.length; i++) {
      (function (npc) {
        var li = document.createElement('li');
        li.className = 'npc'
          + (npc.formId === selectedAgent ? ' selected' : '')
          + (npc.formId === selectedTarget ? ' target-selected' : '');

        var name = document.createElement('span');
        name.className = 'npc-name';
        var raw = npc.name ? npc.name : 'Unknown';
        name.textContent = (raw.length > 45) ? (raw.slice(0, 42) + '...') : raw;

        var dist = document.createElement('span');
        dist.className = 'npc-distance';
        dist.textContent = (npc.distance != null)
          ? (Math.round(npc.distance) + ' u')
          : '';

        // Each row carries two explicit role buttons: Executor (gold 'selected'
        // row + 'active' button) and Target (blue 'target-selected' row +
        // 'active' button). The row itself is no longer clickable, so a role
        // is picked only via a button.
        var execBtn = document.createElement('button');
        execBtn.type = 'button';
        execBtn.className = 'exec-btn'
          + (npc.formId === selectedAgent ? ' active' : '');
        execBtn.textContent = 'Executor';
        execBtn.addEventListener('click', function (ev) {
          ev.stopPropagation();
          selectedAgent = (selectedAgent === npc.formId) ? null : npc.formId;
          drawNearby();
          drawExecSearchResults();
          drawSearchResults();
          updateSummary();
        });

        var targetBtn = document.createElement('button');
        targetBtn.type = 'button';
        targetBtn.className = 'target-btn'
          + (npc.formId === selectedTarget ? ' active' : '');
        targetBtn.textContent = 'Target';
        targetBtn.addEventListener('click', function (ev) {
          ev.stopPropagation();
          selectedTarget = (selectedTarget === npc.formId) ? null : npc.formId;
          drawNearby();
          drawSearchResults();
          updateSummary();
        });

        li.appendChild(name);
        li.appendChild(dist);
        li.appendChild(execBtn);
        li.appendChild(targetBtn);
        ul.appendChild(li);
      })(nearby[i]);
    }
  }

  function renderNearby(list) {
    nearby = Array.isArray(list) ? list : [];

    // An executor or target must survive a nearby refresh while it is still in
    // either the nearby list or its search-result list.
    if (!hasFormId(nearby, selectedAgent) && !hasFormId(execSearchResults, selectedAgent)) {
      selectedAgent = null;
    }
    if (!hasFormId(nearby, selectedTarget) && !hasFormId(searchResults, selectedTarget)) {
      selectedTarget = null;
    }

    drawNearby();
    updateSummary();
  }

  function drawExecSearchResults() {
    var ul = el('exec-search-list');
    ul.textContent = '';

    if (execSearchResults.length === 0) {
      var empty = document.createElement('li');
      empty.className = 'empty';
      empty.textContent = 'No results';
      ul.appendChild(empty);
      return;
    }

    for (var i = 0; i < execSearchResults.length; i++) {
      (function (r) {
        var li = document.createElement('li');
        li.className = 'npc' + (r.formId === selectedAgent ? ' selected' : '');

        var name = document.createElement('span');
        name.className = 'npc-name';
        name.textContent = r.name ? r.name : 'Unknown';

        li.appendChild(name);

        li.addEventListener('click', function () {
          selectedAgent = (selectedAgent === r.formId) ? null : r.formId;
          drawExecSearchResults();
          drawNearby();
          updateSummary();
        });
        ul.appendChild(li);
      })(execSearchResults[i]);
    }
  }

  function renderExecSearchResults(list) {
    execSearchResults = Array.isArray(list) ? list : [];

    // An executor picked from the nearby list is still valid when executor
    // search results refresh or an empty query clears them.
    if (!hasFormId(execSearchResults, selectedAgent) && !hasFormId(nearby, selectedAgent)) {
      selectedAgent = null;
    }

    drawExecSearchResults();
    updateSummary();
  }

  function drawSearchResults() {
    var ul = el('search-list');
    ul.textContent = '';

    if (searchResults.length === 0) {
      var empty = document.createElement('li');
      empty.className = 'empty';
      empty.textContent = 'No results';
      ul.appendChild(empty);
      return;
    }

    for (var i = 0; i < searchResults.length; i++) {
      (function (r) {
        var li = document.createElement('li');
        li.className = 'npc' + (r.formId === selectedTarget ? ' target-selected' : '');

        var name = document.createElement('span');
        name.className = 'npc-name';
        name.textContent = r.name ? r.name : 'Unknown';

        li.appendChild(name);

        // Exclude = add the row's NPC base editorid to the blacklist so the AI
        // resolver never picks it. Hidden when the row carries no editorid.
        if (r.editorid) {
          var excludeBtn = document.createElement('button');
          excludeBtn.type = 'button';
          excludeBtn.className = 'exclude-btn';
          excludeBtn.textContent = 'Exclude';
          excludeBtn.addEventListener('click', function (ev) {
            ev.stopPropagation();
            exclude(r.editorid);
          });
          li.appendChild(excludeBtn);
        }

        li.addEventListener('click', function () {
          selectedTarget = (selectedTarget === r.formId) ? null : r.formId;
          drawSearchResults();
          drawNearby();
          updateSummary();
        });
        ul.appendChild(li);
      })(searchResults[i]);
    }
  }

  // Exclude a row's editorid: blacklist it and drop it from the local result
  // list. The plugin re-pushes the status, which refreshes the Settings block.
  function exclude(editorid) {
    if (!editorid) return;
    send('blacklistAdd', editorid);
    var next = [];
    for (var i = 0; i < searchResults.length; i++) {
      if (searchResults[i].editorid !== editorid) next.push(searchResults[i]);
    }
    renderSearchResults(next);
  }

  function renderSearchResults(list) {
    searchResults = Array.isArray(list) ? list : [];

    // A target picked from the nearby list is still valid when search results
    // refresh or an empty query clears them.
    if (!hasFormId(searchResults, selectedTarget) && !hasFormId(nearby, selectedTarget)) {
      selectedTarget = null;
    }

    drawSearchResults();
    updateSummary();
  }

  function renderTasks(list) {
    tasks = Array.isArray(list) ? list : [];

    var ul = el('active-list');
    ul.textContent = '';

    if (tasks.length === 0) {
      var empty = document.createElement('li');
      empty.className = 'empty';
      empty.textContent = 'No active actions';
      ul.appendChild(empty);
      return;
    }

    for (var i = 0; i < tasks.length; i++) {
      (function (t) {
        var li = document.createElement('li');
        li.className = 'task';

        var main = document.createElement('div');
        main.className = 'task-main';

        var title = document.createElement('div');
        title.className = 'task-title';
        var typeLabel = TYPE_LABELS[t.type] || (t.type || '');
        // For a meet the second name is the meeting place ("Bannered Mare" / "You").
        title.textContent = (typeLabel ? typeLabel + ': ' : '') +
          (t.agent || 'Unknown') + ' -> ' + (t.target || 'Unknown');

        var sub = document.createElement('div');
        sub.className = 'task-detail';
        var label = STATE_LABELS[t.state] || t.state || '';
        sub.textContent = t.detail ? (label + ' - ' + t.detail) : label;

        main.appendChild(title);
        main.appendChild(sub);

        var btn = document.createElement('button');
        btn.type = 'button';
        btn.className = 'cancel-btn';
        btn.textContent = 'Cancel';
        btn.disabled = (t.canCancel === false);
        btn.addEventListener('click', function () {
          send('cancelTask', String(t.id));
          window.setTimeout(refreshTasks, 300);
        });

        li.appendChild(main);
        li.appendChild(btn);
        ul.appendChild(li);
      })(tasks[i]);
    }
  }

  function refreshTasks() {
    send('requestTasks', '');
  }

  function search(role) {
    var isExec = (role === 'executor');
    var input = el(isExec ? 'exec-query' : 'target-query');
    var q = input ? input.value : '';
    q = q ? q.trim() : '';
    if (!q) {
      if (isExec) {
        renderExecSearchResults([]);
      } else {
        renderSearchResults([]);
      }
      return;
    }
    send('searchActors', JSON.stringify({ role: isExec ? 'executor' : 'target', query: q }));
  }

  function execute() {
    var msg = el('exec-message');
    if (selectedAgent == null) {
      msg.textContent = 'Select or search for an executor.';
      return;
    }

    var action = actionDd.value;
    var payload = { action: action, agentFormId: selectedAgent };

    if (action === 'meet') {
      var place = 'Here';
      if (meetModeDd.value === 'Named') {
        place = el('meet-location').value;
        place = place ? place.trim() : '';
        if (!place) {
          msg.textContent = 'Enter a meeting place name.';
          return;
        }
      }
      var hours = parseInt(el('meet-hours').value, 10);
      if (!hours || hours < 1 || hours > 48) {
        msg.textContent = 'Choose a meeting time (1-48 hours).';
        return;
      }
      payload.locationName = place;
      payload.hours = hours;
    } else {
      if (selectedTarget == null) {
        msg.textContent = (action === 'deliver')
          ? 'Select a recipient.'
          : 'Search and select a target person.';
        return;
      }
      if (selectedAgent === selectedTarget) {
        msg.textContent = 'Executor and target must be different.';
        return;
      }
      payload.targetFormId = selectedTarget;
      if (action === 'deliver') {
        var text = el('message-input').value;
        text = text ? text.trim() : '';
        if (!text) {
          msg.textContent = 'Enter a message.';
          return;
        }
        payload.message = text;
        var from = el('message-from').value;
        from = from ? from.trim() : '';
        if (from) payload.sender = from;
      } else if (action === 'assassinate') {
        // An empty or 0 fee is allowed: no fee is paid and the prompt omits
        // the "for N gold" clause.
        var fee = parseFee();
        if (fee > 0) payload.fee = fee;
      }
    }
    msg.textContent = '';

    send('executeAction', JSON.stringify(payload));

    if (action !== 'meet') {
      selectedTarget = null;
      el('message-from').value = '';
      drawSearchResults();
      drawNearby();
      updateSummary();
    }
    window.setTimeout(refreshTasks, 300);
  }

  // The plugin calls these via InvokeJS.
  window.intelite = {
    setStatus: renderStatus,
    setNearby: renderNearby,
    setTasks: renderTasks,
    setSearchResults: renderSearchResults,
    setExecutorSearchResults: renderExecSearchResults
  };

  // The two native <select> controls, replaced by page-rendered dropdowns.
  // Function declarations above hoist, so onChange may name them here.
  var actionDd = makeDropdown('action-select-dd', [
    { value: 'fetch', label: 'Fetch Person' },
    { value: 'deliver', label: 'Deliver Message' },
    { value: 'meet', label: 'Meet Later' },
    { value: 'assassinate', label: 'Assassinate' }
  ], 'fetch', updateActionParams);

  var meetModeDd = makeDropdown('meet-mode-dd', [
    { value: 'Here', label: 'Here' },
    { value: 'Named', label: 'Named' }
  ], 'Here', updateMeetLocationVisibility);

  function activateTab(name) {
    var tabs = document.querySelectorAll('.tab');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].classList.toggle('active', tabs[i].getAttribute('data-tab') === name);
    }
    var panels = document.querySelectorAll('.tab-panel');
    for (var j = 0; j < panels.length; j++) {
      panels[j].classList.toggle('active', panels[j].id === ('tab-' + name));
    }
  }

  document.addEventListener('DOMContentLoaded', function () {
    var tabs = document.querySelectorAll('.tab');
    for (var i = 0; i < tabs.length; i++) {
      tabs[i].addEventListener('click', function () {
        activateTab(this.getAttribute('data-tab'));
      });
    }

    el('exec-search-btn').addEventListener('click', function () { search('executor'); });
    el('exec-query').addEventListener('keydown', function (ev) {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        search('executor');
      }
    });

    el('search-btn').addEventListener('click', function () { search('target'); });
    el('target-query').addEventListener('keydown', function (ev) {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        search('target');
      }
    });

    // The dropdowns replace the old native selects' change listeners; the
    // callbacks passed to makeDropdown fire instead. Close any open menu when
    // clicking elsewhere on the page or pressing Escape.
    document.addEventListener('click', function () { closeDropdowns(null); });
    document.addEventListener('keydown', function (ev) {
      if (ev.key === 'Escape') closeDropdowns(null);
    });

    el('meet-location').addEventListener('input', updateSummary);
    el('meet-hours').addEventListener('input', updateSummary);
    el('fee-input').addEventListener('input', updateSummary);

    el('execute-btn').addEventListener('click', execute);

    el('confirm-toggle').addEventListener('change', function () {
      send('setConfirm', this.checked ? '1' : '0');
    });

    // Settings blacklist: Add button (and Enter in the editorid field).
    function addBlacklistEntry() {
      var input = el('blacklist-input');
      var id = input ? input.value.trim() : '';
      if (!id) return;
      send('blacklistAdd', id);
      input.value = '';
    }
    el('blacklist-add-btn').addEventListener('click', addBlacklistEntry);
    el('blacklist-input').addEventListener('keydown', function (ev) {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        addBlacklistEntry();
      }
    });

    el('close-btn').addEventListener('click', function () {
      send('panelClose', '');
    });

    updateActionParams();

    // Tell the plugin the page is ready; it replies with status + nearby NPCs.
    send('panelReady', '');

    // Pull the current task list, and keep it fresh while the page is loaded
    // (panelReady fires only once; this covers the panel being reopened).
    refreshTasks();
    window.setInterval(function () {
      if (!document.hidden) refreshTasks();
    }, 2500);
    document.addEventListener('visibilitychange', function () {
      if (document.hidden) return;
      refreshTasks();
    });
  });
}());
