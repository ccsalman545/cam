/*
 * janus-client.js
 *
 * Self-contained browser client for the camstream Janus dashboard
 * (no CDN, works on an isolated LAN). Talks to the Janus gateway
 * WebSocket API, attaches to the streaming plugin, watches the
 * camstream mountpoint and answers the plugin's SDP offer with a
 * recvonly RTCPeerConnection.
 *
 * camstream itself is only the media source: it pushes H.264 RTP
 * to the Janus gateway, Janus does the WebRTC.
 *
 * Protocol (Janus 0.x streaming plugin, verified against the
 * official streaming example):
 *
 *   1. {"janus":"create"}                              -> session id
 *   2. {"janus":"attach","plugin":"janus.plugin.streaming"}
 *                                              -> handle id
 *   3. {"janus":"message","body":{"request":"list"}}   -> mountpoints
 *   4. {"janus":"message","body":{"request":"watch","id":N}}
 *        -> event with jsep {"type":"offer","sdp":...}
 *   5. RTCPeerConnection: setRemote(offer), createAnswer,
 *        setLocalDescription, wait for ICE gathering to finish
 *        (no trickle: the answer carries all candidates)
 *   6. {"janus":"message",
 *        "body":{"request":"start"},
 *        "jsep":{"type":"answer","sdp":...}}
 *   7. media flows via ontrack; Janus reports "webrtcup".
 *
 * URL parameters (optional):
 *   ?janus=192.168.1.10:8188     gateway WebSocket host:port
 *                                (default: page hostname + :8188)
 *   ?stream=1                    mountpoint id (default: first
 *                                "rtp" mountpoint in the list)
 *   ?stun=stun:stun.example.com:3478
 *                                ICE server (default: none; direct
 *                                Ethernet setups need host candidates only)
 *
 * Usage:
 *   var client = new CamJanusClient({
 *     video: document.getElementById('video'),
 *     log: function (level, msg) {},
 *     onstate: function (state, detail) {},
 *     onmount: function (mount) {}
 *   });
 *   client.connect();
 *   client.disconnect();
 */
(function (global) {
  'use strict';

  var KEEPALIVE_PERIOD_MS = 20000;
  var TRANSACTION_TIMEOUT_MS = 10000;
  var RECONNECT_DELAY_MS = 3000;
  var GATHERING_TIMEOUT_MS = 3000;

  function queryParam(name) {
    var search = global.location && global.location.search ?
      global.location.search.replace(/^\?/, '') : '';
    var pairs = search.split('&');
    for (var i = 0; i < pairs.length; i++) {
      if (!pairs[i]) continue;
      var kv = pairs[i].split('=');
      if (decodeURIComponent(kv[0]) === name) {
        return decodeURIComponent(kv[1] || '');
      }
    }
    return null;
  }

  function randomId() {
    var out = '';
    for (var i = 0; i < 12; i++) {
      out += Math.floor(Math.random() * 36).toString(36);
    }
    return 'cam' + out;
  }

  /*
   * CamJanusClient
   */
  function CamJanusClient(options) {
    options = options || {};

    this.video = options.video || null;
    this.logFn = options.log || function () {};
    this.stateFn = options.onstate || function () {};
    this.mountFn = options.onmount || function () {};

    var host = queryParam('janus') ||
      ((global.location && global.location.hostname) || '127.0.0.1') + ':8188';
    this.wsUrl = 'ws://' + host;

    var stream = queryParam('stream');
    this.streamId = stream === null ? null :
      (parseInt(stream, 10).toString() === stream ? parseInt(stream, 10) : stream);

    var stun = queryParam('stun');
    this.iceServers = stun ? [{ urls: stun }] : [];

    this.ws = null;
    this.sessionId = null;
    this.handleId = null;
    this.pc = null;
    this.remoteStream = null;
    this.mount = null;
    this.state = 'disconnected';
    this.pending = {};
    this.keepaliveTimer = null;
    this.reconnectTimer = null;
    this.intentionalClose = false;
  }

  CamJanusClient.prototype.log = function (level, msg) {
    this.logFn(level, msg);
  };

  CamJanusClient.prototype.setState = function (state, detail) {
    this.state = state;
    this.stateFn(state, detail);
  };

  /* ---------------------------------------------------------- */
  /* WebSocket + Janus API transport                             */
  /* ---------------------------------------------------------- */

  CamJanusClient.prototype.connect = function () {
    if (this.intentionalClose) {
      return;
    }

    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      if (this.sessionId && this.handleId) {
        this.listStreams();
      } else if (this.sessionId) {
        this.attachPlugin();
      } else {
        this.createSession();
      }
      return;
    }

    this.setState('connecting');
    this.log('info', 'connecting to Janus at ' + this.wsUrl);

    var self = this;

    try {
      this.ws = new WebSocket(this.wsUrl);
    } catch (err) {
      this.log('error', 'cannot create WebSocket: ' + err);
      this.setState('error');
      return;
    }

    this.ws.onopen = function () {
      self.log('info', 'WebSocket open');
      self.createSession();
    };

    this.ws.onmessage = function (ev) {
      self.onMessage(ev);
    };

    this.ws.onerror = function () {
      self.log('warn', 'WebSocket error (see close reason)');
    };

    this.ws.onclose = function (ev) {
      self.log('warn', 'WebSocket closed (code ' + ev.code + ')');
      self.teardownSession(false);
      self.ws = null;

      if (self.intentionalClose) {
        self.setState('disconnected');
        return;
      }

      self.setState('reconnecting');
      self.log('info', 'reconnecting in ' +
        (RECONNECT_DELAY_MS / 1000) + 's');
      if (self.reconnectTimer) {
        clearTimeout(self.reconnectTimer);
      }
      self.reconnectTimer = setTimeout(function () {
        self.reconnectTimer = null;
        if (!self.intentionalClose) {
          self.connect();
        }
      }, RECONNECT_DELAY_MS);
    };
  };

  CamJanusClient.prototype.disconnect = function () {
    this.intentionalClose = true;
    this.log('info', 'stopping');
    this.sendStopRequest();
    this.teardownSession(false);
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      try {
        this.ws.close();
      } catch (err) {
        /* already closing */
      }
    }
    this.setState('disconnected');
  };

  /*
   * Send one Janus API message, remembered by transaction id.
   */
  CamJanusClient.prototype.send = function (type, fields, successCb, label) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      this.log('warn', 'send ' + (label || type) +
        ' dropped (WebSocket not open)');
      return;
    }

    var tx = randomId();
    var msg = { janus: type, transaction: tx };

    for (var key in fields) {
      if (Object.prototype.hasOwnProperty.call(fields, key)) {
        msg[key] = fields[key];
      }
    }

    var self = this;

    msg.timeoutTimer = setTimeout(function () {
      if (!self.pending[tx]) {
        return;
      }
      delete self.pending[tx];
      self.log('warn', 'timeout: no answer to ' + (label || type) +
        ' from Janus (is it running at ' + self.wsUrl + '?)');

      if (label === 'keepalive') {
        /*
         * The gateway went silent: force a reconnect.
         */
        self.log('info', 'forcing reconnect (keepalive lost)');
        try {
          self.ws.close();
        } catch (err) {
          /* already closing */
        }
      } else if (self.state !== 'error' && self.state !== 'disconnected') {
        self.setState('error', (label || type) + ' timeout');
      }
    }, TRANSACTION_TIMEOUT_MS);

    this.pending[tx] = {
      success: successCb || null,
      label: label || type
    };

    try {
      this.ws.send(JSON.stringify(msg));
    } catch (err) {
      delete this.pending[tx];
      this.log('warn', 'send failed: ' + err);
    }
  };

  CamJanusClient.prototype.resolvePending = function (tx) {
    var entry = this.pending[tx];

    if (!entry) {
      return null;
    }

    delete this.pending[tx];
    clearTimeout(entry.timeoutTimer);
    return entry;
  };

  /* ---------------------------------------------------------- */
  /* Inbound messages                                            */
  /* ---------------------------------------------------------- */

  CamJanusClient.prototype.onMessage = function (ev) {
    var msg;

    try {
      msg = JSON.parse(ev.data);
    } catch (err) {
      return;
    }

    if (!msg || typeof msg !== 'object') {
      return;
    }

    /*
     * Global events (no transaction).
     */
    if (msg.janus === 'webrtcup') {
      this.log('info', 'Janus: WebRTC connection is up');
      this.setState('streaming');
      return;
    }

    if (msg.janus === 'hangup') {
      this.log('warn', 'Janus hung up the PeerConnection');
      this.cleanupMedia();
      this.setState('error', 'hangup');
      return;
    }

    if (msg.janus === 'detached') {
      this.log('warn', 'Janus detached the handle');
      this.handleId = null;
      this.cleanupMedia();
      this.setState('error', 'detached');
      return;
    }

    if (msg.janus === 'trickle') {
      this.onTrickleCandidate(msg.candidate);
      return;
    }

    /*
     * Everything else is answered by (or belongs to) a transaction.
     */
    var entry = msg.transaction ? this.resolvePending(msg.transaction) : null;

    if (msg.janus === 'error') {
      var error = msg.error || {};
      this.log('error', 'Janus error ' + error.code + ': ' + error.cause);

      if (!entry && this.state !== 'disconnected') {
        this.setState('error', error.cause);
      }
      return;
    }

    if (entry && entry.success &&
        (msg.janus === 'success' || msg.janus === 'event' ||
         msg.janus === 'ack')) {
      entry.success(msg);
      return;
    }

    if (!entry && msg.janus === 'event') {
      /*
       * Unsolicited plugin event (e.g. a streamed keyframe notice
       * or a second SDP leg): forward to the state machine if it
       * carries an SDP.
       */
      if (msg.jsep && msg.jsep.sdp) {
        this.handlePluginSdp(msg);
      }
    }
  };

  /* ---------------------------------------------------------- */
  /* Janus streaming session flow                                */
  /* ---------------------------------------------------------- */

  CamJanusClient.prototype.createSession = function () {
    var self = this;

    this.send('create', {}, function (msg) {
      var id = msg.data && msg.data.id;

      if (!id) {
        self.log('error', 'create: missing session id');
        self.setState('error');
        return;
      }

      self.sessionId = id;
      self.log('info', 'session ' + id + ' created');
      self.startKeepalive();
      self.attachPlugin();
    }, 'create');
  };

  CamJanusClient.prototype.attachPlugin = function () {
    var self = this;

    this.setState('attaching');
    this.send('attach', {
      plugin: 'janus.plugin.streaming',
      session_id: this.sessionId
    }, function (msg) {
      var id = msg.data && msg.data.id;

      if (!id) {
        self.log('error', 'attach: missing handle id');
        self.setState('error');
        return;
      }

      self.handleId = id;
      self.log('info', 'streaming plugin attached (handle ' + id + ')');
      self.listStreams();
    }, 'attach');
  };

  CamJanusClient.prototype.listStreams = function () {
    var self = this;

    this.setState('listing');
    this.send('message', {
      session_id: this.sessionId,
      handle_id: this.handleId,
      body: { request: 'list' }
    }, function (msg) {
      var plugindata = msg.plugindata || {};
      var data = plugindata.data || {};
      var list = data.list || [];
      var chosen = null;
      var i;

      for (i = 0; i < list.length; i++) {
        if (self.streamId !== null &&
            String(list[i].id) === String(self.streamId)) {
          chosen = list[i];
          break;
        }
        if (self.streamId === null && list[i].type === 'rtp' && !chosen) {
          chosen = list[i];
        }
      }

      if (!chosen && self.streamId !== null) {
        /*
         * Trust the requested id even if "list" did not show it
         * (e.g. a private mountpoint).
         */
        chosen = { id: self.streamId, description: '(forced)' };
      }

      if (!chosen) {
        self.log('error', 'no "rtp" mountpoint found on Janus ' +
          '(configure config/janus/janus.plugin.streaming.jcfg)');
        self.setState('error');
        return;
      }

      self.mount = chosen;
      self.mountFn(chosen);
      self.log('info', 'watching mountpoint ' + chosen.id +
        ' ("' + (chosen.description || '') + '")');
      self.setState('watching');
      self.watchStream(chosen.id);
    }, 'list');
  };

  CamJanusClient.prototype.watchStream = function (id) {
    var self = this;

    this.setState('watching');
    this.send('message', {
      session_id: this.sessionId,
      handle_id: this.handleId,
      body: { request: 'watch', id: id }
    }, function (msg) {
      var plugindata = msg.plugindata || {};
      var data = plugindata.data || {};
      var result = data.result || {};

      self.log('info', 'watch accepted (status: ' +
        (result.status || '?') + ')');

      if (msg.jsep && msg.jsep.sdp) {
        self.handlePluginSdp(msg);
      }
    }, 'watch');
  };

  /*
   * The plugin generated an SDP offer for us (watch flow).
   */
  CamJanusClient.prototype.handlePluginSdp = function (msg) {
    var jsep = msg.jsep || {};

    if (jsep.type !== 'offer' || !jsep.sdp) {
      this.log('warn', 'unexpected jsep (type: ' + jsep.type + ')');
      return;
    }

    this.setState('negotiating');
    this.log('info', 'got SDP offer from Janus, answering');
    this.negotiate(jsep.sdp);
  };

  /* ---------------------------------------------------------- */
  /* WebRTC negotiation (recvonly answer, no trickle)            */
  /* ---------------------------------------------------------- */

  CamJanusClient.prototype.negotiate = function (offerSdp) {
    var self = this;

    this.cleanupMedia();

    var pc;

    try {
      pc = new RTCPeerConnection({ iceServers: this.iceServers });
    } catch (err) {
      this.log('error', 'RTCPeerConnection unavailable: ' + err);
      this.setState('error');
      return;
    }

    this.pc = pc;

    var answerSent = false;
    var localSdpReady = false;

    function sendAnswerNow() {
      if (answerSent) {
        return;
      }
      answerSent = true;
      self.sendAnswer(pc);
    }

    pc.oniceconnectionstatechange = function () {
      var text = pc.iceConnectionState;
      self.log('info', 'ICE state: ' + text);
      self.setState('ice', text);
    };

    pc.onconnectionstatechange = function () {
      self.log('info', 'connection state: ' + pc.connectionState);
    };

    /*
     * No trickle: the complete answer (all candidates in the SDP)
     * goes out once gathering finishes. On a direct link with
     * host-only candidates this is immediate; the timeout below
     * is a safety net only.
     */
    pc.onicecandidate = function (ev) {
      if (ev.candidate === null && localSdpReady) {
        sendAnswerNow();
      }
    };

    pc.ontrack = function (ev) {
      var stream;

      if (ev.streams && ev.streams[0]) {
        stream = ev.streams[0];
      } else if (self.remoteStream) {
        stream = self.remoteStream;
        stream.addTrack(ev.track);
      } else {
        stream = new MediaStream([ev.track]);
      }

      self.remoteStream = stream;

      if (self.video) {
        self.video.srcObject = stream;

        var playPromise = self.video.play();

        if (playPromise && playPromise.catch) {
          playPromise.catch(function () {
            /* autoplay with sound can be blocked; the video is
             * muted in the page, so this should not happen */
          });
        }
      }

      self.log('ok', 'remote video track received');
    };

    if (this.video) {
      this.video.addEventListener('playing', function () {
        var el = self.video;

        if (el.videoWidth > 0) {
          var tag = global.document.getElementById('tag-res');

          if (tag) {
            tag.textContent = el.videoWidth + ' \u00d7 ' + el.videoHeight;
          }
        }
      });
    }

    pc.setRemoteDescription({ type: 'offer', sdp: offerSdp }).then(function () {
      return pc.createAnswer();
    }).then(function (answer) {
      return pc.setLocalDescription(answer);
    }).then(function () {
      localSdpReady = true;

      /*
       * Gathering already finished before we could hook the
       * candidate events (fast host-only case): send right away.
       */
      if (pc.iceGatheringState === 'complete') {
        sendAnswerNow();
        return;
      }

      setTimeout(function () {
        if (!answerSent) {
          self.log('warn', 'ICE gathering timeout, sending answer anyway');
          sendAnswerNow();
        }
      }, GATHERING_TIMEOUT_MS);
    }).catch(function (err) {
      self.log('error', 'WebRTC negotiation failed: ' + err);
      self.setState('error');
    });
  };

  CamJanusClient.prototype.sendAnswer = function (pc) {
    var self = this;
    var sdp = pc.localDescription && pc.localDescription.sdp;

    if (!sdp) {
      this.log('error', 'no local SDP to send');
      this.setState('error');
      return;
    }

    this.log('info', 'sending SDP answer (starting stream)');
    this.send('message', {
      session_id: this.sessionId,
      handle_id: this.handleId,
      body: { request: 'start' },
      jsep: { type: 'answer', sdp: sdp }
    }, function (msg) {
      var plugindata = msg.plugindata || {};
      var data = plugindata.data || {};
      var result = data.result || {};

      self.log('ok', 'stream started (status: ' +
        (result.status || 'started') + ')');

      if (self.state !== 'streaming') {
        self.setState('negotiating');
      }
    }, 'start');
  };

  CamJanusClient.prototype.onTrickleCandidate = function (candidate) {
    var pc = this.pc;

    if (!pc) {
      return;
    }

    if (!candidate || candidate.completed === true) {
      pc.addIceCandidate(null).catch(function () {});
      return;
    }

    this.log('info', 'remote ICE candidate: ' +
      (candidate.candidate || '').slice(0, 60) + '…');

    var self = this;

    pc.addIceCandidate(candidate).catch(function (err) {
      self.log('warn', 'addIceCandidate failed: ' + err);
    });
  };

  CamJanusClient.prototype.sendStopRequest = function () {
    if (!this.sessionId || !this.handleId) {
      return;
    }

    var self = this;

    this.send('message', {
      session_id: this.sessionId,
      handle_id: this.handleId,
      body: { request: 'stop' }
    }, function () {
      self.log('info', 'stop acknowledged');
    }, 'stop');
  };

  /* ---------------------------------------------------------- */
  /* Keepalive + cleanup                                         */
  /* ---------------------------------------------------------- */

  CamJanusClient.prototype.startKeepalive = function () {
    var self = this;

    if (this.keepaliveTimer) {
      return;
    }

    this.keepaliveTimer = setInterval(function () {
      if (self.sessionId === null || !self.ws ||
          self.ws.readyState !== WebSocket.OPEN) {
        return;
      }

      self.send('keepalive', { session_id: self.sessionId }, null,
        'keepalive');
    }, KEEPALIVE_PERIOD_MS);
  };

  CamJanusClient.prototype.cleanupMedia = function () {
    if (this.pc) {
      try {
        this.pc.onicecandidate = null;
        this.pc.ontrack = null;
        this.pc.close();
      } catch (err) {
        /* already closed */
      }
      this.pc = null;
    }

    if (this.video) {
      this.video.srcObject = null;
    }

    this.remoteStream = null;
  };

  CamJanusClient.prototype.teardownSession = function (clearWs) {
    if (this.keepaliveTimer) {
      clearInterval(this.keepaliveTimer);
      this.keepaliveTimer = null;
    }

    for (var tx in this.pending) {
      if (Object.prototype.hasOwnProperty.call(this.pending, tx)) {
        clearTimeout(this.pending[tx].timeoutTimer);
      }
    }

    this.pending = {};
    this.cleanupMedia();
    this.sessionId = null;
    this.handleId = null;
    this.mount = null;
    this.mountFn(null);

    if (clearWs && this.ws) {
      try {
        this.ws.close();
      } catch (err) {
        /* already closing */
      }
    }
  };

  function updateIceText(text) {
    var el = global.document.getElementById('j-ice');

    if (el) {
      el.textContent = text || '–';
      el.className = 'v ' + (text === 'connected' || text === 'completed' ?
        'ok' : text === 'failed' ? 'bad' : '');
    }
  }

  /*
   * Mirror a few states into the dashboard rows as well.
   */
  var _setState = CamJanusClient.prototype.setState;

  CamJanusClient.prototype.setState = function (state, detail) {
    if (state === 'ice') {
      updateIceText(detail);
      return;
    }

    var wsEl = global.document.getElementById('j-webrtc');

    if (wsEl) {
      wsEl.textContent = state;
      wsEl.className = 'v ' + (state === 'streaming' ? 'ok' :
        state === 'error' ? 'bad' : '');
    }

    var sessionEl = global.document.getElementById('j-session');
    var handleEl = global.document.getElementById('j-handle');
    var self = this;

    if (sessionEl) {
      sessionEl.textContent = self.sessionId !== null ?
        String(self.sessionId) : '–';
    }
    if (handleEl) {
      handleEl.textContent = self.handleId !== null ?
        String(self.handleId) : '–';
    }

    _setState.call(this, state, detail);
  };

  global.CamJanusClient = CamJanusClient;
})(window);
