// VirtualCamNative - Unified Core Client Engine (vcam_core.js)
// Shared business logic and hardware API client for all UI skins (Bento & Aqua)

(function () {
    // --- 1. CORE STATE ---
    window.activeConnectionMode = 'none';
    window.savedConnectionMode = 'wifi';
    window.currentPhoneRes = "1080p";
    window.currentPhoneFps = 60;
    window.phoneMaxFps = 60;
    window.phoneCurrentCamera = 'back';
    window.isVertical = true;
    window.isMicMuted = false;
    window.isHudStatsVisible = false;
    window.isRestoringSettings = false;
    window.availableDevices = [];
    window.selectedDevice = null;
    window.savedPairedDevices = JSON.parse(localStorage.getItem('vcam_paired_list') || '{}');
    window.streamReconnectTimeout = null;

    // UI Feedback Helpers
    window.vcamNotify = function (msg, type = 'info') {
        if (typeof showToast === 'function') {
            showToast(msg, type);
        } else if (typeof showAquaToast === 'function') {
            showAquaToast(msg);
        }
    };

    window.vcamClick = function () {
        if (typeof playInterfaceClick === 'function') {
            playInterfaceClick();
        }
    };

    // --- 2. HISTOGRAM ENGINE ---
    let histCanvas = null;
    let histCtx = null;
    let histSampleCanvas = null;
    let histSampleCtx = null;

    window.initHistogram = function () {
        histCanvas = document.getElementById('liveHistogram');
        if (histCanvas) histCtx = histCanvas.getContext('2d');
        histSampleCanvas = document.createElement('canvas');
        histSampleCanvas.width = 96;
        histSampleCanvas.height = 54;
        histSampleCtx = histSampleCanvas.getContext('2d', { willReadFrequently: true });
    };

    window.updateHistogram = function () {
        const videoStream = document.getElementById('videoStream');
        if (!videoStream || videoStream.classList.contains('hidden') || !videoStream.naturalWidth) return;
        if (!histCtx || !histSampleCtx) return;

        try {
            histSampleCtx.drawImage(videoStream, 0, 0, 96, 54);
            const imgData = histSampleCtx.getImageData(0, 0, 96, 54);
            const data = imgData.data;
            const bins = new Uint32Array(256);
            let maxCount = 0;
            let lowClip = 0;
            let highClip = 0;
            const total = 96 * 54;

            for (let i = 0; i < data.length; i += 4) {
                const luma = Math.round(0.2126 * data[i] + 0.7152 * data[i + 1] + 0.0722 * data[i + 2]);
                bins[luma]++;
                if (bins[luma] > maxCount) maxCount = bins[luma];
                if (luma < 5) lowClip++;
                else if (luma > 250) highClip++;
            }

            const box = document.getElementById('hudHistogramBox');
            if (box) box.classList.remove('hidden');

            const w = histCanvas.width;
            const h = histCanvas.height;
            histCtx.clearRect(0, 0, w, h);
            histCtx.fillStyle = 'rgba(0, 0, 0, 0.65)';
            histCtx.fillRect(0, 0, w, h);

            if (maxCount === 0) return;

            const grad = histCtx.createLinearGradient(0, h, 0, 0);
            grad.addColorStop(0, 'rgba(6, 182, 212, 0.15)');
            grad.addColorStop(0.7, 'rgba(6, 182, 212, 0.6)');
            grad.addColorStop(1, 'rgba(236, 72, 153, 0.85)');

            histCtx.beginPath();
            histCtx.moveTo(0, h);
            for (let x = 0; x < w; x++) {
                const binIdx = Math.floor((x / w) * 256);
                const count = bins[binIdx];
                const barH = (count / maxCount) * (h - 2);
                histCtx.lineTo(x, h - barH);
            }
            histCtx.lineTo(w, h);
            histCtx.closePath();
            histCtx.fillStyle = grad;
            histCtx.fill();

            histCtx.strokeStyle = 'rgba(6, 182, 212, 0.85)';
            histCtx.lineWidth = 1;
            histCtx.beginPath();
            for (let x = 0; x < w; x++) {
                const binIdx = Math.floor((x / w) * 256);
                const count = bins[binIdx];
                const barH = (count / maxCount) * (h - 2);
                if (x === 0) histCtx.moveTo(x, h - barH);
                else histCtx.lineTo(x, h - barH);
            }
            histCtx.stroke();

            const badge = document.getElementById('histClipBadge');
            if (badge) {
                if (highClip > total * 0.08) {
                    badge.innerText = 'Overexp ⚠';
                    badge.className = 'text-[9px] text-rose-400 font-bold';
                } else if (lowClip > total * 0.15) {
                    badge.innerText = 'Underexp ⚠';
                    badge.className = 'text-[9px] text-sky-400 font-bold';
                } else {
                    badge.innerText = 'Optimal';
                    badge.className = 'text-[9px] text-emerald-400 font-bold';
                }
            }
        } catch (_) { }
    };

    // --- 3. DEVICE DISCOVERY & CONNECTION ---
    window.updateDevicesDropdownUI = function () {
        const container = document.getElementById('devicesListContainer');
        if (!container) return;
        const dict = (window.translations && window.translations[window.currentLang]) || {};

        if (!window.availableDevices || window.availableDevices.length === 0) {
            container.innerHTML = `<div class="px-3 py-3 text-xs text-zinc-500 text-center">${dict.noDevicesFound || "No devices found"}</div>`;
            if (!window.selectedDevice) {
                const sdt = document.getElementById('selectedDeviceText');
                if (sdt) sdt.innerText = dict.waitingDevice || "Waiting for device...";
            }
            return;
        }

        const isAqua = document.body.classList.contains('aqua-skin') || !!document.getElementById('aqua-traffic-lights');
        if (isAqua) {
            container.innerHTML = window.availableDevices.map(dev => `
                <div onclick="chooseDevice('${dev.ip}')" class="aqua-dropdown-item px-4 py-2 text-gray-900 cursor-pointer flex items-center justify-between border-b border-gray-200">
                    <span class="truncate pr-2 font-bold">${dev.name} <span class="text-gray-500 font-normal ml-1">(${dev.ip})</span></span>
                    ${window.savedPairedDevices[dev.ip] ? `<span class="text-[9px] bg-blue-100 text-blue-800 border border-blue-300 px-1.5 py-0.5 rounded font-bold">${dict.pairedBadge || "PAIRED"}</span>` : ''}
                </div>
            `).join('');
        } else {
            container.innerHTML = window.availableDevices.map(dev => `
                <div onclick="chooseDevice('${dev.ip}')" class="px-3 py-2.5 text-sm text-zinc-300 hover:bg-white/10 rounded-xl cursor-pointer flex items-center justify-between mb-1 transition-colors">
                    <span class="truncate pr-2">${dev.name} <span class="text-zinc-500 text-xs ml-1">(${dev.ip})</span></span>
                    ${window.savedPairedDevices[dev.ip] ? `<span class="text-[9px] bg-koko-red/20 text-koko-red px-2 py-0.5 rounded-full font-bold tracking-wide shrink-0">${dict.pairedBadge || "PAIRED"}</span>` : ''}
                </div>
            `).join('');
        }

        if (!window.selectedDevice && window.availableDevices.length > 0) {
            window.chooseDevice(window.availableDevices[0].ip);
        }
    };

    window.chooseDevice = function (ip) {
        window.vcamClick();
        window.selectedDevice = window.availableDevices.find(d => d.ip === ip);
        if (window.selectedDevice) {
            const sdt = document.getElementById('selectedDeviceText');
            if (sdt) sdt.innerText = `${window.selectedDevice.name}`;
            if (typeof closeAllDropdowns === 'function') closeAllDropdowns();
        }
    };

    window.openPairedModal = function () {
        if (typeof closeAllDropdowns === 'function') closeAllDropdowns();
        const list = document.getElementById('pairedDevicesList');
        const keys = Object.keys(window.savedPairedDevices);
        const dict = (window.translations && window.translations[window.currentLang]) || {};

        if (list) {
            if (keys.length === 0) {
                list.innerHTML = `<div class="text-xs text-zinc-500 py-4 text-center">${dict.noPairedDevices || "No saved devices"}</div>`;
            } else {
                list.innerHTML = keys.map(ip => `
                    <div class="flex items-center justify-between p-2 bg-white/5 rounded-2xl border border-white/5 text-xs mb-2">
                        <div class="overflow-hidden pr-2">
                            <b class="text-white block truncate text-sm">${window.savedPairedDevices[ip].name}</b>
                            <span class="text-[11px] text-zinc-500">${window.savedPairedDevices[ip].ip}</span>
                        </div>
                        <button onclick="forgetDevice('${ip}')" class="px-3 py-1.5 text-[10px] border border-red-900/60 bg-red-900/20 text-red-400 hover:bg-red-900/40 rounded-full font-bold tracking-widest uppercase transition-colors shrink-0">${dict.forgetBtn || "Forget"}</button>
                    </div>
                `).join('');
            }
        }
        const modal = document.getElementById('pairedModal');
        if (modal) modal.classList.remove('hidden');
    };

    window.closePairedModal = function () {
        const modal = document.getElementById('pairedModal');
        if (modal) modal.classList.add('hidden');
    };

    window.forgetDevice = function (ip) {
        delete window.savedPairedDevices[ip];
        localStorage.setItem('vcam_paired_list', JSON.stringify(window.savedPairedDevices));
        window.openPairedModal();
        window.updateDevicesDropdownUI();
    };

    // Smart Connect Handler (USB -> Wi-Fi)
    window.triggerSmartConnect = async function () {
        const btn = document.getElementById('btnSmartConnect');
        if (btn) {
            btn.disabled = true;
            btn.classList.add('opacity-70');
        }
        window.vcamNotify("Smart Connect: поиск и подключение...", "active");

        // 1. Попытка подключения по USB (ADB)
        try {
            let res = await fetch('http://127.0.0.1:8000/api/connect_adb', { method: 'POST', headers: { 'Accept': 'application/json' } });
            let data = await res.json();
            if (data.status === 'connected') {
                window.setConnected(true, 'usb');
                window.vcamNotify("Подключено по USB (ADB)", "active");
                if (btn) { btn.disabled = false; btn.classList.remove('opacity-70'); }
                return;
            }
        } catch (_) { }

        // 2. Попытка подключения по Wi-Fi
        let targetDev = window.selectedDevice;
        if (!targetDev) {
            const pairedKeys = Object.keys(window.savedPairedDevices);
            if (pairedKeys.length > 0) {
                targetDev = window.savedPairedDevices[pairedKeys[0]];
            }
        }

        if (targetDev && targetDev.ip) {
            const ip = targetDev.ip;
            let token = window.savedPairedDevices[ip] ? window.savedPairedDevices[ip].token : null;

            if (!token) {
                const modal = document.getElementById('pairingModal');
                if (modal) modal.classList.remove('hidden');
                try {
                    let pairRes = await fetch(`http://127.0.0.1:8000/api/pair_device?ip=${ip}`, { method: 'POST' });
                    let pairData = await pairRes.json();
                    if (modal) modal.classList.add('hidden');
                    if (pairData.status === 'paired' && pairData.auth_token) {
                        token = pairData.auth_token;
                        window.savedPairedDevices[ip] = { name: targetDev.name || ip, ip: ip, token: token };
                        localStorage.setItem('vcam_paired_list', JSON.stringify(window.savedPairedDevices));
                        window.updateDevicesDropdownUI();
                    }
                } catch (_) {
                    if (modal) modal.classList.add('hidden');
                }
            }

            try {
                let res = await fetch('http://127.0.0.1:8000/api/connect', {
                    method: 'POST', headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ip: ip, auth_token: token })
                });
                let data = await res.json();
                if (data.status === 'connected') {
                    window.setConnected(true, 'wifi');
                    window.vcamNotify(`Подключено по Wi-Fi: ${targetDev.name || ip}`, "active");
                    if (btn) { btn.disabled = false; btn.classList.remove('opacity-70'); }
                    return;
                }
            } catch (_) { }
        }

        if (btn) { btn.disabled = false; btn.classList.remove('opacity-70'); }
        window.vcamNotify("Подключите смартфон через USB или выберите в списке устройств", "error");
    };

    window.setConnected = function (connected, mode = null) {
        window.vcamClick();
        const btnDisconnect = document.getElementById('btnDisconnect');
        if (btnDisconnect) btnDisconnect.disabled = !connected;
        const dict = (window.translations && window.translations[window.currentLang]) || {};

        const btnSmart = document.getElementById('btnSmartConnect');
        const liveControls = document.getElementById('liveStreamControls');
        const statusDot = document.getElementById('statusDot');
        const statusText = document.getElementById('statusText');
        const videoStream = document.getElementById('videoStream');
        const videoPlaceholder = document.getElementById('videoPlaceholder');
        const statsHud = document.getElementById('statsHud');

        if (mode) {
            window.activeConnectionMode = mode;
            window.savedConnectionMode = mode;
        }

        if (connected) {
            if (btnSmart) btnSmart.classList.add('hidden');
            if (liveControls) liveControls.classList.remove('hidden');

            if (statusDot) {
                statusDot.classList.replace('bg-red-500', 'bg-emerald-500');
                statusDot.classList.replace('border-red-800', 'border-emerald-800');
                statusDot.classList.replace('shadow-[0_0_8px_rgba(239,68,68,0.8)]', 'shadow-[0_0_8px_rgba(16,185,129,0.8)]');
            }
            if (statusText) statusText.innerText = dict.connected || "Connected";

            setTimeout(() => {
                if (typeof onPhoneSettingChanged === 'function') onPhoneSettingChanged();
                if (typeof pollPhoneStatus === 'function') pollPhoneStatus();
            }, 200);

            setTimeout(() => {
                if (videoStream) {
                    videoStream.src = "http://127.0.0.1:8000/stream?t=" + Date.now();
                    videoStream.classList.remove('hidden');
                    if (typeof safeAnime === 'function') {
                        safeAnime({ targets: videoStream, opacity: [0, 1], duration: 800, easing: 'easeOutSine' });
                    } else {
                        videoStream.style.opacity = '1';
                    }
                }
                if (videoPlaceholder) videoPlaceholder.classList.add('hidden');
                if (statsHud) {
                    if (window.isHudStatsVisible) statsHud.classList.remove('hidden');
                    else statsHud.classList.add('hidden');
                }
                if (typeof resetZenTimer === 'function') resetZenTimer();
            }, 700);

        } else {
            window.activeConnectionMode = 'none';
            clearTimeout(window.streamReconnectTimeout);
            if (btnSmart) btnSmart.classList.remove('hidden');
            if (liveControls) liveControls.classList.add('hidden');

            const dock = document.getElementById('capsuleDock');
            if (dock) { dock.style.opacity = '1'; dock.style.pointerEvents = 'auto'; }

            if (videoStream) {
                videoStream.removeAttribute('src');
                videoStream.src = "";
                videoStream.classList.add('hidden');
                videoStream.style.opacity = '0';
            }
            if (videoPlaceholder) videoPlaceholder.classList.remove('hidden');
            if (statsHud) statsHud.classList.add('hidden');
            if (statusDot) {
                statusDot.classList.replace('bg-emerald-500', 'bg-red-500');
                statusDot.classList.replace('border-emerald-800', 'border-red-800');
                statusDot.classList.replace('shadow-[0_0_8px_rgba(16,185,129,0.8)]', 'shadow-[0_0_8px_rgba(239,68,68,0.8)]');
            }
            if (statusText) statusText.innerText = dict.disconnected || "Disconnected";
        }
    };

    // --- 4. PHONE ACTIONS & SETTINGS ---
    window.sendPhoneAction = async function (actionName) {
        try {
            await fetch(`http://127.0.0.1:8000/api/phone/action/${actionName}`, {
                method: 'POST', headers: { 'Accept': 'application/json' }
            });
            if (actionName === 'switch_camera' && typeof pollPhoneStatus === 'function') {
                setTimeout(pollPhoneStatus, 600);
            }
        } catch (_) { }
    };

    window.toggleQuickMic = async function () {
        window.isMicMuted = !window.isMicMuted;
        await window.sendPhoneAction('toggle_mic_mute');
        const btn = document.getElementById('btnQuickMute');
        const icon = document.getElementById('iconQuickMute');
        if (btn && icon) {
            if (window.isMicMuted) {
                btn.classList.add('bg-rose-500/20', 'text-rose-400', 'border-rose-500/30');
                btn.classList.remove('bg-white/5', 'text-zinc-300');
                icon.setAttribute('href', '#icon-mic-off');
                window.vcamNotify("Микрофон: ВЫКЛЮЧЕН", "warning");
            } else {
                btn.classList.remove('bg-rose-500/20', 'text-rose-400', 'border-rose-500/30');
                btn.classList.add('bg-white/5', 'text-zinc-300');
                icon.setAttribute('href', '#icon-mic');
                window.vcamNotify("Микрофон: ВКЛЮЧЕН", "active");
            }
        }
    };

    let g_quickTorchOn = false;
    window.toggleQuickTorch = async function () {
        g_quickTorchOn = !g_quickTorchOn;
        await window.sendPhoneAction('toggle_torch');
        const btn = document.getElementById('btnQuickTorch');
        if (btn) {
            if (g_quickTorchOn) {
                btn.classList.add('bg-amber-500/20', 'text-amber-400', 'border-amber-500/30');
                btn.classList.remove('bg-white/5', 'text-zinc-300');
                window.vcamNotify("Фонарик: ВКЛ", "active");
            } else {
                btn.classList.remove('bg-amber-500/20', 'text-amber-400', 'border-amber-500/30');
                btn.classList.add('bg-white/5', 'text-zinc-300');
                window.vcamNotify("Фонарик: ВЫКЛ", "info");
            }
        }
    };

    window.setOrientation = function (vertical) {
        if (window.isVertical === vertical) return;
        window.isVertical = vertical;
        if (typeof updateOrientationUI === 'function') updateOrientationUI();
        fetch(`http://127.0.0.1:8000/api/orientation?mode=${window.isVertical ? "vertical" : "horizontal"}`).catch(() => { });
        window.updateSettingsLive();
    };

    window.setAspectRatio = function (mode) {
        window.currentAspectRatioMode = parseInt(mode) || 0;
        if (typeof updateAspectRatioUI === 'function') updateAspectRatioUI();
        fetch(`http://127.0.0.1:8000/api/aspect_ratio?mode=${window.currentAspectRatioMode}`).catch(() => { });
        if (typeof updateSettingsLive === 'function') updateSettingsLive();
    };

    // --- 5. AUDIO DSP & OPTICS ---
    window.onVolumeChange = function (val) {
        const vav = document.getElementById('valAudioVolume');
        if (vav) vav.innerText = val + "%";
        fetch('http://127.0.0.1:8000/api/audio_volume?val=' + (val / 100.0)).catch(() => { });
        window.updateSettingsLive();
    };

    window.onAudioDelayChange = function (val) {
        const el = document.getElementById('valAudioDelay');
        if (el) el.innerText = val + " ms";
        fetch(`http://127.0.0.1:8000/api/audio_delay?val=${val}`).catch(() => { });
        window.updateSettingsLive();
    };

    window.setAudioDelayPreset = function (val) {
        const slider = document.getElementById('audioDelay');
        if (slider) {
            slider.value = val;
            window.onAudioDelayChange(val);
        }
    };

    window.onNoiseGateToggle = function (enabled) {
        if (window.isRestoringSettings) return;
        fetch(`http://127.0.0.1:8000/api/audio_noisegate?enabled=${enabled}`).catch(() => { });
        window.updateSettingsLive();
        window.vcamNotify(enabled ? "Noise Gate: Включен" : "Noise Gate: Выключен", enabled ? "active" : "mute");
    };

    window.onAiNoiseToggle = function (checked) {
        if (window.isRestoringSettings) return;
        fetch(`http://127.0.0.1:8000/api/audio_ai_noise?enabled=${checked}`).catch(() => { });
        window.updateSettingsLive();
        window.vcamNotify(checked ? "RNNoise AI: Active" : "RNNoise AI: Off", checked ? "active" : "mute");
    };

    window.onAgcToggle = function (checked) {
        if (window.isRestoringSettings) return;
        fetch(`http://127.0.0.1:8000/api/audio_agc?enabled=${checked}`).catch(() => { });
        window.updateSettingsLive();
        window.vcamNotify(checked ? "AGC Leveler: Active" : "AGC: Off", checked ? "active" : "mute");
    };

    window.onDeclickerToggle = function (checked) {
        if (window.isRestoringSettings) return;
        fetch(`http://127.0.0.1:8000/api/audio_declicker?enabled=${checked}`).catch(() => { });
        window.updateSettingsLive();
        window.vcamNotify(checked ? "Switch De-Clicker: Active" : "De-Clicker: Off", checked ? "active" : "mute");
    };

    window.onEqChange = function () {
        if (window.isRestoringSettings) return;
        const low = document.getElementById('eqLow')?.value || 0;
        const mid = document.getElementById('eqMid')?.value || 0;
        const high = document.getElementById('eqHigh')?.value || 0;
        const vl = document.getElementById('valEqLow');
        const vm = document.getElementById('valEqMid');
        const vh = document.getElementById('valEqHigh');
        if (vl) vl.innerText = (low > 0 ? '+' : '') + low + ' dB';
        if (vm) vm.innerText = (mid > 0 ? '+' : '') + mid + ' dB';
        if (vh) vh.innerText = (high > 0 ? '+' : '') + high + ' dB';
        fetch(`http://127.0.0.1:8000/api/audio_eq?low=${low}&mid=${mid}&high=${high}`).catch(() => { });
        window.updateSettingsLive();
    };

    // --- 6. POLLING INTERVALS ---
    setInterval(async () => {
        if (window.activeConnectionMode === 'none') {
            try {
                let res = await fetch('http://127.0.0.1:8000/api/devices');
                window.availableDevices = await res.json();
                window.updateDevicesDropdownUI();
            } catch (_) { }
        }
    }, 2000);

    setInterval(async () => {
        const videoStream = document.getElementById('videoStream');
        if (videoStream && !videoStream.classList.contains('hidden')) {
            try {
                const res = await fetch('http://127.0.0.1:8000/api/telemetry');
                const t = await res.json();
                const hudFps = document.getElementById('hudFps');
                const hudBitrate = document.getElementById('hudBitrate');
                const hudCodec = document.getElementById('hudCodec');
                if (hudFps) hudFps.innerText = `${t.fps.toFixed(1)} FPS`;
                if (hudBitrate) hudBitrate.innerText = t.bitrate;
                if (hudCodec) hudCodec.innerText = t.codec;
                window.updateHistogram();
            } catch (_) { }
        }
    }, 500);

    // Initial setup on script load
    document.addEventListener('DOMContentLoaded', () => {
        window.initHistogram();
        if (typeof applyTranslations === 'function') applyTranslations();
    });
})();
