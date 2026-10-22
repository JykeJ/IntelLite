/* IntelLite consent prompt page.
 *
 * C++ -> page:
 *   window.intelitePrompt.show({kind, text, agent, target, place, message, location, hours, fee})
 *   window.intelitePrompt.setCandidates([{name, distance, formId}])
 *   window.intelitePrompt.setSearchResults([{name, formId, editorid}])
 *
 * page -> C++ (window.magelight.send):
 *   'promptAgree' | 'promptDecline' | 'promptChange'
 *   'promptSearch' (query) | 'promptPickTarget' (formId)
 *   'promptSetMessage' (text) | 'promptSetLocation' (name)
 *   'promptSetSender' (text) | 'promptSetHours' (n) | 'promptSetFee' (n)
 *   'blacklistAdd' (editorid, a search row's Exclude button)
 */
(function () {
  'use strict';

  function el(id) { return document.getElementById(id); }

  function send(name, arg) {
    if (window.magelight && typeof window.magelight.send === 'function') {
      window.magelight.send(name, arg === undefined ? '' : String(arg));
    }
  }

  var payload = {};          // { kind, text, agent, target, place, message, location, hours }
  var candidates = [];       // [{ name, distance, formId }]
  var results = [];          // [{ name, formId }]
  var selectedName = '';     // highlight by name (the payload carries no target FormID)

  function showScreen(which) {
    el('screen-confirm').classList.toggle('active', which === 'confirm');
    el('screen-change').classList.toggle('active', which === 'change');
  }

  // Show only the change controls that belong to the current kind.
  function applyKind() {
    var kind = payload.kind || 'fetch';
    var targetKind = (kind === 'fetch' || kind === 'deliver' || kind === 'assassinate');
    el('change-target').style.display = targetKind ? '' : 'none';
    el('change-message').style.display = (kind === 'deliver') ? '' : 'none';
    el('change-meet').style.display = (kind === 'meet') ? '' : 'none';
    el('change-fee').style.display = (kind === 'assassinate') ? '' : 'none';
    if (kind === 'deliver') {
      el('prompt-message').value = payload.message || '';
      el('prompt-message-from').value = payload.sender || '';
    } else {
      el('prompt-message-from').value = '';
    }
    if (kind === 'meet') {
      // An empty location arrives only when the player cleared the Named
      // field on this screen: keep the mode on Named so a place can be typed.
      var loc = (typeof payload.location === 'string') ? payload.location : 'Here';
      var named = (loc !== 'Here');
      el('prompt-location-mode').value = named ? 'Named' : 'Here';
      el('prompt-location').value = named ? loc : '';
      el('prompt-location-row').style.display = named ? '' : 'none';
      el('prompt-hours').value = payload.hours || 2;
    }
    if (kind === 'assassinate') {
      el('prompt-fee').value = (payload.fee != null ? payload.fee : 500);
    }
  }

  function drawList(listId, list, emptyText, withExclude) {
    var ul = el(listId);
    ul.textContent = '';

    if (!list.length) {
      var empty = document.createElement('li');
      empty.className = 'empty';
      empty.textContent = emptyText;
      ul.appendChild(empty);
      return;
    }

    for (var i = 0; i < list.length; i++) {
      (function (row) {
        var li = document.createElement('li');
        li.className = 'npc' + (row.name === selectedName ? ' selected' : '');

        var name = document.createElement('span');
        name.className = 'npc-name';
        name.textContent = row.name ? row.name : 'Unknown';
        li.appendChild(name);

        if (row.distance != null) {
          var dist = document.createElement('span');
          dist.className = 'npc-distance';
          dist.textContent = Math.round(row.distance) + ' u';
          li.appendChild(dist);
        }

        // Exclude = blacklist the row's NPC base editorid. Shown on search
        // results only, and only when the row carries an editorid.
        if (withExclude && row.editorid) {
          var excludeBtn = document.createElement('button');
          excludeBtn.type = 'button';
          excludeBtn.className = 'exclude-btn';
          excludeBtn.textContent = 'Exclude';
          excludeBtn.addEventListener('click', function (ev) {
            ev.stopPropagation();
            send('blacklistAdd', row.editorid);
            var next = [];
            for (var k = 0; k < results.length; k++) {
              if (results[k].editorid !== row.editorid) next.push(results[k]);
            }
            setSearchResults(next);
          });
          li.appendChild(excludeBtn);
        }

        li.addEventListener('click', function () {
          selectedName = row.name || '';
          send('promptPickTarget', row.formId);
        });
        ul.appendChild(li);
      })(list[i]);
    }
  }

  // ---- C++ -> page ---------------------------------------------------------

  function show(p) {
    payload = (p && typeof p === 'object') ? p : {};
    selectedName = payload.target || '';
    el('prompt-text').textContent = payload.text || '';
    candidates = [];
    results = [];
    el('prompt-query').value = '';
    drawList('prompt-candidates', candidates, 'No nearby people');
    drawList('prompt-results', results, 'No search yet', true);
    applyKind();
    showScreen('confirm');
  }

  function setCandidates(list) {
    candidates = Array.isArray(list) ? list : [];
    drawList('prompt-candidates', candidates, 'No nearby people');
  }

  function setSearchResults(list) {
    results = Array.isArray(list) ? list : [];
    drawList('prompt-results', results, 'No results', true);
  }

  window.intelitePrompt = {
    show: show,
    setCandidates: setCandidates,
    setSearchResults: setSearchResults
  };

  function search() {
    var q = el('prompt-query').value;
    q = q ? q.trim() : '';
    if (!q) {
      setSearchResults([]);
      return;
    }
    send('promptSearch', q);
  }

  document.addEventListener('DOMContentLoaded', function () {
    el('agree-btn').addEventListener('click', function () { send('promptAgree', ''); });
    el('decline-btn').addEventListener('click', function () { send('promptDecline', ''); });
    el('change-btn').addEventListener('click', function () {
      send('promptChange', '');
      showScreen('change');
    });
    el('back-btn').addEventListener('click', function () { showScreen('confirm'); });

    el('prompt-search-btn').addEventListener('click', search);
    el('prompt-query').addEventListener('keydown', function (ev) {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        search();
      }
    });

    el('prompt-message').addEventListener('change', function () {
      send('promptSetMessage', this.value);
    });
    el('prompt-message-from').addEventListener('change', function () {
      send('promptSetSender', this.value);
    });
    el('prompt-location-mode').addEventListener('change', function () {
      var named = this.value === 'Named';
      el('prompt-location-row').style.display = named ? '' : 'none';
      send('promptSetLocation', named ? el('prompt-location').value : 'Here');
    });
    el('prompt-location').addEventListener('change', function () {
      send('promptSetLocation', this.value);
    });
    el('prompt-hours').addEventListener('change', function () {
      send('promptSetHours', this.value);
    });
    el('prompt-fee').addEventListener('change', function () {
      send('promptSetFee', this.value);
    });

    // Escape = decline (the host may also turn Escape into a UI-mode exit,
    // which the plugin treats as a decline too).
    document.addEventListener('keydown', function (ev) {
      if (ev.key === 'Escape') {
        ev.preventDefault();
        send('promptDecline', '');
      }
    });
  });
}());
