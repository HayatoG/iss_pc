#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""inFAMOUS Second Son launcher (Tkinter, Linux and Windows), in pt_BR and en_US.

Game folder, game and interface language, display, FPS overlay and VRAM budget; Play starts the
game through infamous_run.py (the same code path as run_infamous.sh) and shows its log.
Settings live in ~/.config/infamous-launcher/settings.json (%APPDATA% on Windows).
"""
import locale
import multiprocessing
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, simpledialog, ttk

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backports_table  # noqa: E402
import infamous_run as tlg_run  # noqa: E402  (same API as the TLG core)
import logtail  # noqa: E402
import precompile  # noqa: E402
import preset_share  # noqa: E402

LOG_LINES = 500           # lines the log panel keeps
LOG_POLL_MS = 1000        # how often the game's log file is read
CLOSE_DELAY_MS = 2000     # 'Close the launcher when the game starts': wait after the start

TEXT = {
    'title': ('inFAMOUS Second Son — PC', 'inFAMOUS Second Son — PC'),
    'game': ('Game', 'Jogo'),
    'game_dir': ('Game folder', 'Pasta do jogo'),
    'browse': ('Browse…', 'Procurar…'),
    'language': ('Language', 'Idioma'),
    'game_language': ('Game language (voice and text)', 'Idioma do jogo (voz e texto)'),
    'game_language_auto': ('Automatic (system language)', 'Automático (idioma do sistema)'),
    'game_language_now': ('Automatic: {name}', 'Automático: {name}'),
    'ui_language': ('Interface language', 'Idioma da interface'),
    'auto': ('Automatic (system)', 'Automático (sistema)'),
    'video': ('Video', 'Vídeo'),
    'display_mode': ('Display mode', 'Modo de exibição'),
    'Windowed': ('Windowed', 'Janela'),
    'Borderless': ('Borderless window', 'Janela sem borda'),
    'Fullscreen': ('Fullscreen', 'Tela cheia'),
    'vsync': ('VSync', 'VSync'),
    'show_fps': ('Show FPS overlay at start (F10 toggles)', 'Mostrar overlay de FPS ao iniciar (F10 alterna)'),
    'eager_readback': ('Read GPU results ahead (faster; no effect in renderer v2)',
                       'Ler resultados da GPU antecipado (mais rápido; sem efeito no renderer v2)'),
    'write_merge': ('Write to GPU memory without waiting (faster; no effect in renderer v2)',
                    'Escrever na memória da GPU sem esperar (mais rápido; sem efeito no renderer v2)'),
    'vram': ('Texture cache budget (MB of VRAM)', 'Orçamento do cache de texturas (MB de VRAM)'),
    'vram_hint': ('6 GB card: 3500 · 8 GB: 5000 · 12 GB: 8500 · 16 GB: 12000 · 24 GB: 19000. This is video card memory (VRAM), not RAM; never above your card\'s VRAM.',
                  'Placa de 6 GB: 3500 · 8 GB: 5000 · 12 GB: 8500 · 16 GB: 12000 · 24 GB: 19000. É a memória da placa de vídeo (VRAM), não a RAM; nunca acima da VRAM da placa.'),
    'gpu_readbacks': ('GPU accuracy (no effect in renderer v2)', 'Precisão da GPU (sem efeito no renderer v2)'),
    'readbacks_0': ('Fast (no GPU particles)', 'Rápido (sem partículas da GPU)'),
    'readbacks_1': ('Relaxed', 'Intermediário'),
    'readbacks_2': ('Precise (recommended)', 'Preciso (recomendado)'),
    'controller': ('Controller', 'Controle'),
    'deadzone': ('Stick dead zone', 'Zona morta dos analógicos'),
    'deadzone_hint': ('Raise it if the character or camera moves on its own (stick drift).',
                      'Aumente se o personagem ou a câmera se mexer sozinho (drift).'),
    'sensor_hint': ('Motion sensor: DualShock/DualSense gyro works as is. Without a sensor, press L3+R3 '
                    '(or Share on Xbox Series) for sensor mode (the graffiti spray can, held sideways): cross/A shakes, R2 sprays, left stick aims; L3+R3 again '
                    'turns it off. Touchpad: View/Select (hold + right stick = swipe; + R1 = both ends). Keyboard T/G/Y.',
                    'Sensor de movimento: o giroscópio do DualShock/DualSense funciona direto. Sem sensor, aperte L3+R3 '
                    '(ou Share no Xbox Series) para o modo sensor (a lata de spray do grafite, já de lado): X/A sacode, R2 pinta, analógico esquerdo mira; '
                    'L3+R3 de novo desliga. Touchpad: View/Select (segurar + analógico direito = deslizar; + R1 = as duas pontas). '
                    'Teclado T/G/Y.'),
    'play': ('Play', 'Jogar'),
    'saves': ('Saves folder', 'Pasta de saves'),
    'logs': ('Logs folder', 'Pasta de logs'),
    'clear_cache': ('Clear shader cache', 'Limpar cache de shaders'),
    'clear_cache_ask': ('This deletes the compiled shaders ({size}); the next start takes longer to load and '
                        'may stutter at first. Continue?',
                        'Isso apaga os shaders compilados ({size}); a próxima partida demora mais para carregar e '
                        'pode engasgar no começo. Continuar?'),
    'clear_cache_empty': ('The shader cache is already empty.', 'O cache de shaders já está vazio.'),
    'clear_cache_done': ('Shader cache cleared: {size} freed.', 'Cache de shaders limpo: {size} liberados.'),
    'clear_cache_failed': ('{size} freed, but {count} file(s) could not be deleted (in use? Close the game '
                           'and try again).',
                           '{size} liberados, mas {count} arquivo(s) não puderam ser apagados (em uso? Feche o '
                           'jogo e tente de novo).'),
    'mods': ('Mods / Cheats…', 'Mods / Cheats…'),
    'mods_title': ('Mods / Cheats', 'Mods / Cheats'),
    'mods_hint': ('Drop shadPS4/GoldHEN cheat XML files in the mods folder, then turn them on here. '
                  'The built-in boot intro skip is off by default. Nothing changes until you enable a mod.',
                  'Solte arquivos de cheat XML no formato shadPS4/GoldHEN na pasta de mods e ligue aqui. '
                  'O patch incluído para pular o vídeo inicial vem desligado. Nada muda até você ativar um mod.'),
    'mods_warning': ('A cheat can crash the game or behave oddly; if that happens, turn it off here.',
                     'Um cheat pode travar o jogo ou se comportar de forma estranha; se isso acontecer, desligue aqui.'),
    'mods_folder': ('Open mods folder', 'Abrir pasta de mods'),
    'mods_refresh': ('Refresh list', 'Atualizar lista'),
    'mods_empty': ('No cheat XML files found in the mods folder yet.',
                   'Nenhum arquivo de cheat XML encontrado na pasta de mods ainda.'),
    'close': ('Close', 'Fechar'),
    'quit': ('Quit', 'Sair'),
    'ready': ('Ready to play.', 'Pronto para jogar.'),
    'missing': ('Missing', 'Faltando'),
    'not_built': ('Port not built (run build.sh)', 'Port não compilado (rode o build.sh)'),
    'preparing': ('Preparing the game…', 'Preparando o jogo…'),
    'running': ('Playing — log:', 'Jogando — log:'),
    'ended': ('Game closed (exit code {code}).', 'Jogo fechado (código {code}).'),
    'crashed': ('The game closed with an error (code {code}). The log has the details.',
                'O jogo fechou com erro (código {code}). Os detalhes estão no log.'),
    'already': ('The game is already running.', 'O jogo já está rodando.'),
    'error': ('Error', 'Erro'),
    'port_by': ('PC port by HayatoG', 'Port para PC por HayatoG'),
    'coffee': ('Buy me a coffee', 'Apoie com um café'),
    'channel': ('YouTube channel: @hayatog-dev', 'Canal no YouTube: @hayatog-dev'),
    'discord': ('Discord community', 'Comunidade no Discord'),
    'sponsor': ('♥ Sponsor on GitHub', '♥ Apoie no GitHub'),
    'experimental': ('Experimental fixes…', 'Correções experimentais…'),
    'exp_title': ('Experimental fixes and game menu options', 'Correções experimentais e opções do menu do jogo'),
    'exp_intro': ('Everything here is saved in tlg.ini and applies the next time the game starts. Items tagged "in game" can also be changed while playing, in the Insert menu. If you do not touch anything, the recommended profile (the table defaults) is used.', 'Tudo aqui é salvo no tlg.ini e vale na próxima vez que o jogo abrir. Os itens com a etiqueta "no jogo" também podem ser mudados durante a partida, no menu Insert. Se você não mexer em nada, vale o perfil recomendado (os padrões do port).'),
    'exp_tab_menu': ('Game menu (Insert)', 'Menu do jogo (Insert)'),
    'exp_tab_bp': ('Experimental fixes (BPnn)', 'Correções experimentais (BPnn)'),
    'exp_tab_ab': ('Diagnostic / A-B switches', 'Chaves de diagnóstico / A-B'),
    'exp_menu_note': ('Display mode, VSync and the FPS overlay switch are in the Video box of the main window. The options below are the other ones of the Insert menu.', 'Modo de exibição, VSync e o botão do overlay de FPS ficam no quadro Vídeo da janela principal. Abaixo estão as outras opções do menu Insert.'),
    'exp_bp_note': ('Fixes taken from other PS4 ports and from shadPS4. Those tagged "experimental" are off by default: turn them on only to test.', 'Correções tiradas de outros ports de PS4 e do shadPS4. As marcadas como "experimental" vêm desligadas: ligue só para testar.'),
    'exp_ab_note': ('Switches for finding which change causes a problem. Defaults are what the port uses. "Automatic" lets the GPU code decide for your driver.', 'Chaves para descobrir qual mudança causa um problema. Os padrões são os que o port usa. "Automático" deixa o código da GPU decidir para o seu driver.'),
    'exp_tab_Image': ('Image', 'Imagem'),
    'exp_tab_View': ('Draw distance and detail', 'Distância de visão e detalhe'),
    'exp_tab_Performance': ('Performance', 'Desempenho'),
    'exp_tab_Controls': ('Controls', 'Controle'),
    'exp_tab_Overlay': ('Overlay', 'Overlay'),
    'exp_live': ('in game', 'no jogo'),
    'exp_restart': ('restarts the game', 'reinicia o jogo'),
    'exp_experimental': ('experimental', 'experimental'),
    'exp_broken': ('still crashes the game', 'ainda derruba o jogo'),
    'exp_shader': ('the first game after changing it recompiles the shaders (may flicker or look green for a while)', 'a primeira partida depois de mudar recompila os shaders (pode piscar ou ficar verde por um tempo)'),
    'exp_source': ('source: {source}', 'origem: {source}'),
    'exp_default': ('default', 'padrão'),
    'occlusion': ('Light glare (occlusion)', 'Brilho das luzes (oclusão)'),
    'occlusion_hint': ('How much of a light the game counts as visible. 3 (recommended) keeps the headlight glow from showing through characters; 0 is the old behaviour. Applies on the next start.',
                       'Quanto de uma luz o jogo conta como visível. 3 (recomendado) evita o brilho dos faróis aparecer por cima dos personagens; 0 é o comportamento antigo. Vale na próxima partida.'),
    'close_on_play': ('Close the launcher when the game starts', 'Fechar o launcher ao abrir o jogo'),
    'verbose_log': ('Detailed log', 'Log detalhado'),
    'verbose_log_hint': ('For problem reports: the log records much more (files, controller, audio, GPU with time and thread). It gets large and the game may run a little slower. Applies on the next start.',
                         'Para relatar problemas: o log registra muito mais (arquivos, controle, áudio, GPU com horário e thread). Ele fica grande e o jogo pode ficar um pouco mais lento. Vale na próxima partida.'),
    'exp_auto': ('Automatic', 'Automático'),
    'exp_on': ('On', 'Ligado'),
    'exp_off': ('Off', 'Desligado'),
    'exp_defaults': ('Back to recommended', 'Voltar ao recomendado'),
    'exp_recommended': ('recommended', 'recomendado'),
    'exp_profile_rec': ('Profile: Recommended', 'Perfil: Recomendado'),
    'exp_profile_custom': ('Profile: custom ({n} changes)', 'Perfil: personalizado ({n} mudanças)'),
    'notice': ('This version brings new fixes, already on in the recommended mode. If you notice anything odd (lights, textures, freezes), open Experimental fixes and use "Like the previous release"; then tell us what you saw.',
               'Esta versão traz correções novas, já ativadas no modo recomendado. Se notar algo estranho (luzes, texturas, travamentos), abra Correções experimentais e use Como a release anterior; depois conte o que viu.'),
    'notice_open': ('Open fixes', 'Abrir correções'),
    'notice_ok': ('OK', 'OK'),
    'exp_previous': ('Like the previous release (turn all BPnn off)', 'Como a release anterior (desliga todos os BPnn)'),
    'exp_previous_hint': ('"Like the previous release" writes bp01=0 ... for every fix that is on by default (the same as starting with TLG_BACKPORTS=0); the boxes show it, and you can turn single ones back on.', '"Como a release anterior" grava bp01=0 ... em todas as correções que vêm ligadas (é o mesmo que iniciar com TLG_BACKPORTS=0); as caixas mostram isso e você pode religar uma a uma.'),
    'exp_close': ('Close', 'Fechar'),
}
TEXT.update(preset_share.TEXT)   # export / import of configuration files
TEXT.update(precompile.TEXT)   # shader list texts: one table for both launchers
TEXT.update(tlg_run.TEXT)  # game folder problems (old libc.prx, other eboot), shared with the command line
COFFEE_URL = 'https://buymeacoffee.com/hayatog'
CHANNEL_URL = 'https://www.youtube.com/@hayatog-dev'
DISCORD_URL = 'https://discord.gg/UMkdMYZh9g'
SPONSOR_URL = 'https://github.com/sponsors/HayatoG'
GAME_LANGUAGE_NAMES = {'en': 'English', 'en_GB': 'English (UK)', 'pt_BR': 'Português (Brasil)',
                       'pt_PT': 'Português (Portugal)', 'es_LA': 'Español (Latinoamérica)', 'es': 'Español (España)',
                       'fr_CA': 'Français (Canada)', 'fr': 'Français', 'de': 'Deutsch', 'it': 'Italiano',
                       'nl': 'Nederlands', 'pl': 'Polski', 'tr': 'Türkçe', 'ru': 'Русский', 'cs': 'Čeština',
                       'hu': 'Magyar', 'el': 'Ελληνικά', 'ro': 'Română', 'fi': 'Suomi', 'sv': 'Svenska', 'da': 'Dansk',
                       'no': 'Norsk', 'ar': 'العربية', 'ja': '日本語', 'ko': '한국어', 'zh_TW': '繁體中文',
                       'zh_CN': '简体中文', 'th': 'ไทย', 'vi': 'Tiếng Việt', 'id': 'Bahasa Indonesia'}
UI_LANGUAGES = ['auto', 'pt_BR', 'en_US']
UI_LANGUAGE_NAMES = {'pt_BR': 'Português (Brasil)', 'en_US': 'English (US)'}
DISPLAY_MODES = ['Windowed', 'Borderless', 'Fullscreen']


def art_path(settings, name):
    """Official art from the user's own dump (sce_sys); never shipped with the port."""
    for folder in (Path(settings['game_dir']) / 'sce_sys', tlg_run.REPO_DIR / 'dump' / 'sce_sys'):
        if (folder / name).is_file():
            return folder / name
    return None


def system_is_portuguese():
    for name in ('LC_ALL', 'LC_MESSAGES', 'LANG'):
        value = os.environ.get(name)
        if value:
            return value.lower().startswith('pt')
    try:
        return (locale.getlocale()[0] or '').lower().startswith('pt')
    except ValueError:
        return False


class Launcher:
    def __init__(self, root):
        self.root = root
        self.settings = tlg_run.load_settings()
        self.process = None
        self.tail = None
        self.precompiler = precompile.Precompiler()
        self.starting = False    # Play pressed, game not yet running (preparing / making the shader list)
        self._jobs_step = {}
        self.lines = queue.Queue()
        self.build()
        self.root.after(100, self.pump)
        if backports_table.notice_pending(self.settings):
            self.root.after(400, self.show_notice)

    # -- language ---------------------------------------------------------------------------
    def pt(self):
        choice = self.settings.get('ui_language', 'auto')
        return system_is_portuguese() if choice == 'auto' else choice == 'pt_BR'

    def t(self, key, **fmt):
        en, pt = TEXT[key]
        return (pt if self.pt() else en).format(**fmt)

    # -- window -----------------------------------------------------------------------------
    def build(self):
        """Horizontal layout: banner on top; Game | Language + Video | game log; buttons below."""
        for child in self.root.winfo_children():
            child.destroy()
        root = self.root
        root.title(self.t('title'))
        root.minsize(1120, 640)
        pad = {'padx': 6, 'pady': 4}
        main = ttk.Frame(root, padding=12)
        main.pack(fill='both', expand=True)
        for column, weight in enumerate((3, 3, 4)):
            main.columnconfigure(column, weight=weight, uniform='cols' if column < 2 else '')
        main.rowconfigure(2, weight=1)

        header = ttk.Frame(main)
        header.grid(row=0, column=0, columnspan=3, sticky='ew', pady=(0, 8))
        self.load_art()
        if self.banner:
            ttk.Label(header, image=self.banner).pack(side='left', padx=(0, 14))
        titles = ttk.Frame(header)
        titles.pack(side='left', fill='y')
        ttk.Label(titles, text='inFAMOUS Second Son', font=('TkDefaultFont', 22, 'bold')).pack(anchor='w')
        ttk.Label(titles, text='PC — CUSA00223 v1.07', foreground='#777').pack(anchor='w')
        credits = ttk.Frame(header)
        credits.pack(side='right', anchor='ne')
        ttk.Label(credits, text=self.t('port_by'), foreground='#777').pack(anchor='e')
        self.coffee_button = tk.Button(credits, text='☕  ' + self.t('coffee'),
                                       command=lambda: tlg_run.open_external(COFFEE_URL),
                                       background='#FFDD00', foreground='#000000',
                                       activebackground='#FFE873', activeforeground='#000000',
                                       font=('TkDefaultFont', 10, 'bold'), cursor='hand2',
                                       relief='flat', padx=12, pady=7)
        self.coffee_button.pack(anchor='e', pady=(6, 8))
        for key, url in (('channel', CHANNEL_URL), ('discord', DISCORD_URL), ('sponsor', SPONSOR_URL)):
            link = ttk.Label(credits, text=self.t(key), foreground='#2a6fdb', cursor='hand2',
                             font=('TkDefaultFont', 10, 'underline'))
            link.pack(anchor='e')
            link.bind('<Button-1>', lambda _, u=url: tlg_run.open_external(u))
        self.status = ttk.Label(titles, text='', foreground='#555', wraplength=420, justify='left')
        self.status.pack(anchor='w', pady=(10, 0))

        self.vars = {}
        left = ttk.Frame(main)
        left.grid(row=2, column=0, sticky='nsew', **pad)
        left.columnconfigure(0, weight=1)
        game = ttk.LabelFrame(left, text=self.t('game'), padding=8)
        game.grid(sticky='ew', pady=(0, 8))
        game.columnconfigure(0, weight=1)
        for row, key in enumerate(('game_dir',)):
            ttk.Label(game, text=self.t(key)).grid(row=row * 2, column=0, columnspan=2, sticky='w')
            var = tk.StringVar(value=self.settings[key])
            var.trace_add('write', lambda *_: self.changed())
            self.vars[key] = var
            ttk.Entry(game, textvariable=var).grid(row=row * 2 + 1, column=0, sticky='ew', pady=(0, 6))
            ttk.Button(game, text=self.t('browse'), command=lambda k=key: self.browse(k)).grid(
                row=row * 2 + 1, column=1, padx=(6, 0), pady=(0, 6))

        pad_box = ttk.LabelFrame(left, text=self.t('controller'), padding=8)
        pad_box.grid(sticky='ew', pady=(0, 8))
        pad_box.columnconfigure(0, weight=1)
        ttk.Label(pad_box, text=self.t('deadzone')).grid(row=0, column=0, columnspan=2, sticky='w')
        zone = tk.IntVar(value=int(self.settings.get('stick_deadzone', 10)))
        self.vars['stick_deadzone'] = zone
        zone_text = ttk.Label(pad_box, text=f'{zone.get()}%', width=5)
        zone_text.grid(row=1, column=1, sticky='e')

        def zone_moved(value):
            zone.set(int(round(float(value))))
            zone_text.configure(text=f'{zone.get()}%')
            self.changed()
        ttk.Scale(pad_box, from_=0, to=40, variable=zone, command=zone_moved).grid(row=1, column=0, sticky='ew')
        ttk.Label(pad_box, text=self.t('deadzone_hint'), foreground='#777', wraplength=260, justify='left').grid(
            row=2, column=0, columnspan=2, sticky='w')
        ttk.Label(pad_box, text=self.t('sensor_hint'), foreground='#777', wraplength=260, justify='left').grid(
            row=3, column=0, columnspan=2, sticky='w', pady=(6, 0))

        middle = ttk.Frame(main)
        middle.grid(row=2, column=1, sticky='nsew', **pad)
        middle.columnconfigure(0, weight=1)
        lang = ttk.LabelFrame(middle, text=self.t('language'), padding=8)
        lang.grid(sticky='ew', pady=(0, 8))
        lang.columnconfigure(1, weight=1)
        self.game_language_now = ttk.Label(lang)
        self.game_language_now.grid(row=0, column=2, sticky='w')
        self.game_language_keys = tlg_run.game_language_choices(self.settings)   # the languages the game files have
        self.game_language_box = self.combo(lang, 0, 'game_language', self.game_language_keys, self.game_language_name,
                                            on_change=self.show_game_language)
        self.show_game_language()
        self.combo(lang, 1, 'ui_language', UI_LANGUAGES,
                   lambda k: self.t('auto') if k == 'auto' else UI_LANGUAGE_NAMES[k], on_change=self.build)
        video = ttk.LabelFrame(middle, text=self.t('video'), padding=8)
        video.grid(sticky='ew')
        video.columnconfigure(1, weight=1)
        self.combo(video, 0, 'display_mode', DISPLAY_MODES, lambda k: self.t(k))
        self.check(video, 1, 'vsync')
        self.check(video, 2, 'show_fps')
        self.combo(video, 5, 'gpu_readbacks', ['0', '1', '2'], lambda k: self.t('readbacks_' + k))
        self.check(video, 6, 'eager_readback')
        self.check(video, 7, 'write_merge')
        self.check(video, 8, 'precompile_shaders')
        self.check(video, 9, 'close_on_play')
        ttk.Label(video, text=self.t('vram'), wraplength=220).grid(row=3, column=0, sticky='w', pady=(6, 0))
        vram = tk.IntVar(value=int(self.settings['vram_budget_mb']))
        self.vars['vram_budget_mb'] = vram
        spin = ttk.Spinbox(video, from_=2000, to=24000, increment=200, textvariable=vram, width=8,
                           command=self.changed)
        spin.grid(row=3, column=1, sticky='w', padx=6, pady=(6, 0))
        spin.bind('<FocusOut>', lambda _: self.changed())
        ttk.Label(video, text=self.t('vram_hint'), foreground='#777', wraplength=360).grid(row=4, column=0, columnspan=2, sticky='w')

        self.exp_states = tlg_run.experimental_states(self.settings)
        occ_item = next(i for i in backports_table.ITEMS if i.key == 'occlusion_mode')
        occ_box = ttk.LabelFrame(middle, text=self.t('occlusion'), padding=8)
        occ_box.grid(sticky='ew', pady=(8, 0))
        occ_box.columnconfigure(0, weight=1)
        occ_options = [(v, (pt if self.pt() else en) + (f' ({self.t("exp_default")})' if v == occ_item.default else ''))
                       for v, en, pt in occ_item.choices]
        occ_value = backports_table.effective(occ_item, self.exp_states)
        occ_combo = ttk.Combobox(occ_box, values=[o[1] for o in occ_options], state='readonly')
        occ_combo.current(next((i for i, o in enumerate(occ_options) if o[0] == occ_value), 0))
        occ_combo.grid(row=0, column=0, sticky='ew')
        occ_combo.bind('<<ComboboxSelected>>',
                       lambda _, o=occ_options, b=occ_combo, i=occ_item: self.exp_set(i, o[b.current()][0]))
        ttk.Label(occ_box, text=self.t('occlusion_hint'), foreground='#777', wraplength=360,
                  justify='left').grid(row=1, column=0, sticky='w', pady=(6, 0))

        # Detailed log, right under the light glare box (same file/env path as the other switches).
        log_item = next(i for i in backports_table.ITEMS if i.key == 'verbose_log')
        log_box = ttk.LabelFrame(middle, text=self.t('verbose_log'), padding=8)
        log_box.grid(sticky='ew', pady=(8, 0))
        log_box.columnconfigure(0, weight=1)
        log_var = tk.BooleanVar(value=backports_table.effective(log_item, self.exp_states) == '1')
        ttk.Checkbutton(log_box, text=self.t('verbose_log'), variable=log_var,
                        command=lambda v=log_var, i=log_item: self.exp_set(i, '1' if v.get() else '0')
                        ).grid(row=0, column=0, sticky='w')
        ttk.Label(log_box, text=self.t('verbose_log_hint'), foreground='#777', wraplength=360,
                  justify='left').grid(row=1, column=0, sticky='w', pady=(6, 0))

        log_frame = ttk.LabelFrame(main, text='Log', padding=4)
        log_frame.grid(row=2, column=2, sticky='nsew', **pad)
        self.log = tk.Text(log_frame, wrap='none', font=('TkFixedFont', 9), state='disabled', width=60)
        scroll = ttk.Scrollbar(log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        self.log.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')

        buttons = ttk.Frame(main)
        buttons.grid(row=3, column=0, columnspan=3, sticky='ew', pady=(10, 0))
        ttk.Button(buttons, text=self.t('quit'), command=self.quit).pack(side='left')
        style = ttk.Style()
        style.configure('Play.TButton', font=('TkDefaultFont', 13, 'bold'))
        self.play_button = ttk.Button(buttons, text=self.t('play'), style='Play.TButton', command=self.play)
        self.play_button.pack(side='right', ipadx=30, ipady=8)
        ttk.Button(buttons, text=self.t('experimental'), command=self.open_experimental).pack(side='left', padx=12)
        ttk.Button(buttons, text=self.t('logs'), command=lambda: self.open(tlg_run.LOG_DIR)).pack(side='right', padx=6)
        ttk.Button(buttons, text=self.t('saves'), command=lambda: self.open(tlg_run.user_dir() / 'savedata')).pack(side='right')
        self.cache_button = ttk.Button(buttons, text=self.t('clear_cache'), command=self.clear_cache)
        self.cache_button.pack(side='right', padx=(0, 6))
        ttk.Button(buttons, text=self.t('mods'), command=self.mods_dialog).pack(side='right', padx=(0, 6))
        self.reported = ()  # new (empty) log: report the folder's problems again
        self.refresh_status()

    def load_art(self):
        """Window icon (the game's icon0.png, else the port's infamous.png) and a 480x135 banner (a band of pic1.png from the dump, when present)."""
        self.banner = getattr(self, 'banner', None)
        if getattr(self, 'art_loaded', False):
            return
        self.art_loaded = True
        try:
            icon = art_path(self.settings, 'icon0.png')  # the game's own icon, from the user's dump
            if icon and icon.is_file():
                self.icon = tk.PhotoImage(file=str(icon)).subsample(4)
                self.root.iconphoto(True, self.icon)
            elif (tlg_run.PORT_DIR / 'launcher' / 'infamous.png').is_file():  # the port's own icon
                self.icon = tk.PhotoImage(file=str(tlg_run.PORT_DIR / 'launcher' / 'infamous.png'))
                self.root.iconphoto(True, self.icon)
            picture = art_path(self.settings, 'pic1.png') or art_path(self.settings, 'pic0.png')
            if picture:
                full = tk.PhotoImage(file=str(picture))
                band = full.width() * 9 // 32           # a 32:9 band from the middle of the image
                top = (full.height() - band) // 2
                self.banner = tk.PhotoImage()
                self.banner.tk.call(self.banner, 'copy', full, '-from', 0, top, full.width(), top + band,
                                    '-subsample', 4, 4)  # Tk command: same on every Python version
        except tk.TclError:
            self.banner = None

    def combo(self, parent, row, key, values, label, on_change=None):
        ttk.Label(parent, text=self.t(key)).grid(row=row, column=0, sticky='w')
        names = [label(v) for v in values]
        box = ttk.Combobox(parent, values=names, state='readonly', width=28)
        current = self.settings.get(key, values[0])
        box.current(values.index(current) if current in values else 0)
        box.grid(row=row, column=1, sticky='w', padx=6, pady=2)

        def picked(_):
            self.settings[key] = values[box.current()]
            tlg_run.save_settings(self.settings)
            if on_change:
                on_change()
            else:
                self.refresh_status()
        box.bind('<<ComboboxSelected>>', picked)
        return box

    def game_language_name(self, key):
        return self.t('game_language_auto') if key == 'auto' else GAME_LANGUAGE_NAMES[key]

    def refresh_game_languages(self):
        """Another game folder: offer the languages its files have (combo() reads the same list)."""
        keys = tlg_run.game_language_choices(self.settings)
        if keys != self.game_language_keys:
            self.game_language_keys[:] = keys
            self.game_language_box.configure(values=[self.game_language_name(k) for k in keys])
            current = self.settings.get('game_language', 'auto')
            self.game_language_box.current(keys.index(current) if current in keys else 0)
            self.show_game_language()

    def show_game_language(self):
        """Beside the box: which language 'Automatic' resolves to (same function that build_env uses)."""
        if self.settings.get('game_language', 'auto') in tlg_run.GAME_LANGUAGES:
            self.game_language_now.config(text='')
        else:
            name = GAME_LANGUAGE_NAMES[tlg_run.resolve_game_language(self.settings)]
            self.game_language_now.config(text=self.t('game_language_now', name=name))
        if hasattr(self, 'play_button'):   # called while the window is still being built: the button comes later
            self.refresh_status()

    def mods_dialog(self):
        """Mods/cheats (shadPS4/GoldHEN XML in tlg_run.mods_dir()); toggles write mods.json
        through tlg_run.set_mod_enabled. Nothing here is on unless the player turns it on."""
        if getattr(self, 'mods_window', None) and self.mods_window.winfo_exists():
            self.mods_window.lift()
            return
        win = self.mods_window = tk.Toplevel(self.root)
        win.title(self.t('mods_title'))
        win.transient(self.root)
        win.minsize(560, 360)
        frame = ttk.Frame(win, padding=12)
        frame.pack(fill='both', expand=True)
        ttk.Label(frame, text=self.t('mods_hint'), wraplength=520, justify='left').pack(anchor='w')
        ttk.Label(frame, text=self.t('mods_warning'), foreground='#a33', wraplength=520, justify='left').pack(
            anchor='w', pady=(4, 8))

        list_frame = ttk.Frame(frame)
        list_frame.pack(fill='both', expand=True)
        canvas = tk.Canvas(list_frame, highlightthickness=0)
        scroll = ttk.Scrollbar(list_frame, orient='vertical', command=canvas.yview)
        rows = ttk.Frame(canvas)
        rows.bind('<Configure>', lambda _: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.create_window((0, 0), window=rows, anchor='nw')
        canvas.configure(yscrollcommand=scroll.set)
        canvas.pack(side='left', fill='both', expand=True)
        scroll.pack(side='right', fill='y')

        def toggle(key, var):
            tlg_run.set_mod_enabled(key, var.get())

        def populate():
            for child in rows.winfo_children():
                child.destroy()
            try:
                mods = tlg_run.list_mods(self.settings)
            except Exception as error:  # a malformed XML file is reported, never crashes the launcher
                mods = []
                ttk.Label(rows, text=str(error), foreground='#a33').pack(anchor='w')
            if not mods:
                ttk.Label(rows, text=self.t('mods_empty'), foreground='#777').pack(anchor='w', pady=8)
            for key, name, note, enabled in mods:
                row = ttk.Frame(rows)
                row.pack(fill='x', anchor='w', pady=2)
                var = tk.BooleanVar(value=enabled)
                ttk.Checkbutton(row, text=name or key, variable=var,
                                command=lambda k=key, v=var: toggle(k, v)).pack(anchor='w')
                if note:
                    ttk.Label(row, text=note, foreground='#777', wraplength=480, justify='left').pack(
                        anchor='w', padx=(22, 0))
        populate()

        bottom = ttk.Frame(frame)
        bottom.pack(fill='x', pady=(10, 0))
        ttk.Button(bottom, text=self.t('mods_folder'), command=lambda: self.open(tlg_run.mods_dir())).pack(side='left')
        ttk.Button(bottom, text=self.t('mods_refresh'), command=populate).pack(side='left', padx=6)
        ttk.Button(bottom, text=self.t('close'), command=win.destroy).pack(side='right')

    def check(self, parent, row, key):
        var = tk.BooleanVar(value=bool(self.settings[key]))
        self.vars[key] = var
        ttk.Checkbutton(parent, text=self.t(key), variable=var, command=self.changed).grid(
            row=row, column=0, columnspan=3, sticky='w', pady=2)

    # -- actions ----------------------------------------------------------------------------
    def changed(self):
        for key, var in self.vars.items():
            try:
                self.settings[key] = var.get()
            except tk.TclError:
                pass  # spinbox while typing
        tlg_run.save_settings(self.settings)
        self.refresh_game_languages()
        self.refresh_status()

    def refresh_status(self):
        problems = tlg_run.check_game(self.settings['game_dir'])
        texts = [self.t('not_built') if k == 'not_built' else self.t(k, detail=d) if k in tlg_run.TEXT
                 else f"{self.t('missing')}: {d}" for k, d in problems]
        if not tlg_run.blocking(problems):
            texts.append(self.t('ready'))
        self.status.configure(text='\n'.join(texts), foreground='#b00020' if problems else '#555')
        self.play_button.configure(state='disabled' if tlg_run.blocking(problems) or self.process or self.starting else 'normal')
        reported = tuple(problems)
        if reported != getattr(self, 'reported', ()):  # each new problem also goes to the log once
            self.reported = reported
            for (key, _), text in zip(problems, texts):
                if key in tlg_run.TEXT:
                    self.append(text)
        self.cache_button.configure(state='disabled' if self.process or self.starting else 'normal')

    def browse(self, key):
        path = filedialog.askdirectory(initialdir=self.vars[key].get() or str(Path.home()))
        if path:
            self.vars[key].set(path)

    def open(self, path):
        Path(path).mkdir(parents=True, exist_ok=True)
        if os.name == 'nt':
            os.startfile(path)  # noqa: S606 (local folder)
        else:
            subprocess.Popen(['xdg-open', str(path)])

    def clear_cache(self):
        """Clear shader cache: asks first, never while the game runs; shows how much was freed."""
        if self.process or self.starting:
            messagebox.showinfo(self.t('title'), self.t('already'))
            return
        size = tlg_run.shader_cache_size(self.settings)
        if not size:
            messagebox.showinfo(self.t('clear_cache'), self.t('clear_cache_empty'))
            return
        if not messagebox.askyesno(self.t('clear_cache'),
                                   self.t('clear_cache_ask', size=tlg_run.shader_cache.human_size(size))):
            return
        try:
            freed, failures = tlg_run.clear_shader_cache(self.settings)
        except Exception as error:  # shown to the user, the launcher keeps running
            messagebox.showerror(self.t('error'), str(error))
            return
        freed = tlg_run.shader_cache.human_size(freed)
        if failures:
            message = self.t('clear_cache_failed', size=freed, count=len(failures))
            messagebox.showwarning(self.t('clear_cache'), message)
        else:
            message = self.t('clear_cache_done', size=freed)
            messagebox.showinfo(self.t('clear_cache'), message)
        self.append(message)

    # -- experimental fixes window ---------------------------------------------------------
    def open_experimental(self):
        window = getattr(self, 'exp_window', None)
        if window is not None and window.winfo_exists():
            window.lift()
            return
        window = self.exp_window = tk.Toplevel(self.root)
        window.title(self.t('exp_title'))
        window.geometry('860x640')
        window.transient(self.root)
        self.exp_states = tlg_run.experimental_states(self.settings)
        self.build_experimental()

    def show_notice(self):
        """Once after the update: the new fixes are on; how to go back. The key is saved first, so
        a second start never repeats it."""
        backports_table.mark_notice_seen(self.settings)
        tlg_run.save_settings(self.settings)
        box = tk.Toplevel(self.root)
        box.title(self.t('title'))
        box.transient(self.root)
        ttk.Label(box, text=self.t('notice'), wraplength=440, justify='left', padding=14).pack()
        row = ttk.Frame(box, padding=(14, 0, 14, 12))
        row.pack(fill='x')
        ttk.Button(row, text=self.t('notice_ok'), command=box.destroy).pack(side='right')
        ttk.Button(row, text=self.t('notice_open'),
                   command=lambda: (box.destroy(), self.open_experimental())).pack(side='right', padx=8)

    def exp_status_text(self):
        n = backports_table.changes(self.exp_states)
        return self.t('exp_profile_rec') if n == 0 else self.t('exp_profile_custom', n=n)

    def exp_refresh_status(self):
        label = getattr(self, 'exp_status', None)
        if label is not None and label.winfo_exists():
            label.configure(text=self.exp_status_text())

    def exp_name(self, pair):
        return pair[1] if self.pt() else pair[0]

    def exp_save(self):
        tlg_run.save_experimental(self.settings, self.exp_states)

    def exp_set(self, item, value):
        self.exp_states[item.id] = backports_table.normalize(item, value)
        self.exp_save()
        self.exp_refresh_status()

    def exp_reset(self, which):
        if which == 'previous':
            self.exp_states = backports_table.previous_release(self.exp_states)
        else:
            self.exp_states = backports_table.defaults(self.exp_states)
        self.exp_save()
        self.build_experimental()

    def exp_port_version(self):
        try:
            from qt import report
            return report.current_version((tlg_run.PORT_DIR, tlg_run.REPO_DIR), tlg_run.LOG_DIR)
        except Exception:
            return ''

    def exp_export(self):
        parent = getattr(self, 'exp_window', None) or self.root
        name = simpledialog.askstring(self.t('ps_export_title'), self.t('ps_name'), parent=parent)
        if name is None:
            return
        what = simpledialog.askstring(self.t('ps_export_title'), self.t('ps_what'), parent=parent) or ''
        path = filedialog.asksaveasfilename(parent=parent, title=self.t('ps_save_title'), defaultextension='.json',
                                            initialfile=preset_share.default_filename(name),
                                            filetypes=[(self.t('ps_filter'), '*.json')])
        if not path:
            return
        preset = preset_share.export_preset(self.settings, self.exp_states, name, what, self.exp_port_version())
        try:
            final = preset_share.write_preset(preset, path)
        except OSError as error:
            messagebox.showerror(self.t('ps_export_title'), self.t('ps_export_failed', detail=str(error)), parent=parent)
            return
        messagebox.showinfo(self.t('ps_export_title'), self.t('ps_exported', path=str(final)), parent=parent)

    def exp_import(self):
        parent = getattr(self, 'exp_window', None) or self.root
        path = filedialog.askopenfilename(parent=parent, title=self.t('ps_open_title'),
                                          filetypes=[(self.t('ps_filter'), '*.json'), ('*', '*')])
        if not path:
            return
        title = self.t('ps_import_title')
        preset, problems = preset_share.read_preset(path)
        if preset is None:
            messagebox.showerror(title, '\n'.join(preset_share.problem_text(p, self.t) for p in problems), parent=parent)
            return
        changes = preset_share.diff(preset, self.settings, self.exp_states)
        body, lines = preset_share.summary_lines(preset, problems, changes, self.t, self.pt())
        text = '\n'.join([body, ''] + ['• ' + line for line in lines])
        if not changes:
            messagebox.showinfo(title, text, parent=parent)
            return
        if not messagebox.askyesno(title, text + '\n\n' + self.t('ps_apply') + '?', parent=parent):
            return
        preset_share.apply_preset(preset, self.settings, self.exp_states)
        self.exp_save()
        self.build_experimental()
        messagebox.showinfo(title, self.t('ps_imported', name=preset['profile'], n=len(changes)), parent=parent)

    def scrolled(self, parent):
        canvas = tk.Canvas(parent, highlightthickness=0, height=400)
        bar = ttk.Scrollbar(parent, command=canvas.yview)
        inner = ttk.Frame(canvas, padding=(8, 4))
        window = canvas.create_window((0, 0), window=inner, anchor='nw')
        canvas.configure(yscrollcommand=bar.set)
        inner.bind('<Configure>', lambda _: canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>', lambda e: canvas.itemconfigure(window, width=e.width))
        for sequence, step in (('<Button-4>', -1), ('<Button-5>', 1)):
            canvas.bind_all(sequence, lambda _, s=step: canvas.yview_scroll(s, 'units') if canvas.winfo_exists() else None)
        canvas.bind_all('<MouseWheel>', lambda e: canvas.yview_scroll(-1 if e.delta > 0 else 1, 'units')
                        if canvas.winfo_exists() else None)
        bar.pack(side='right', fill='y')
        canvas.pack(side='left', fill='both', expand=True)
        return inner

    def build_experimental(self):
        window = self.exp_window
        for child in window.winfo_children():
            child.destroy()
        window.title(self.t('exp_title'))
        top = ttk.Frame(window, padding=10)
        top.pack(fill='both', expand=True)
        ttk.Label(top, text=self.t('exp_intro'), wraplength=800, justify='left').pack(anchor='w', pady=(0, 6))
        self.exp_status = ttk.Label(top, text=self.exp_status_text(), font=('TkDefaultFont', 10, 'bold'))
        self.exp_status.pack(anchor='w', pady=(0, 4))
        notebook = ttk.Notebook(top)
        notebook.pack(fill='both', expand=True)
        groups = (('menu', 'exp_tab_menu', 'exp_menu_note'), ('bp', 'exp_tab_bp', 'exp_bp_note'),
                  ('ab', 'exp_tab_ab', 'exp_ab_note'))
        for group, title_key, note_key in groups:
            page = ttk.Frame(notebook)
            notebook.add(page, text=self.t(title_key))
            ttk.Label(page, text=self.t(note_key), wraplength=780, justify='left', foreground='#555').pack(
                anchor='w', padx=8, pady=(6, 0))
            inner = self.scrolled(page)
            tab = None
            for item in backports_table.ITEMS:
                if item.group != group:
                    continue
                if item.tab and item.tab != tab:
                    tab = item.tab
                    ttk.Label(inner, text=self.t('exp_tab_' + tab), font=('TkDefaultFont', 10, 'bold')).pack(
                        anchor='w', pady=(10, 2))
                self.exp_row(inner, item)
        buttons = ttk.Frame(top)
        buttons.pack(fill='x', pady=(8, 0))
        ttk.Label(buttons, text=self.t('exp_previous_hint'), wraplength=800, justify='left', foreground='#555').pack(
            anchor='w', pady=(0, 6))
        ttk.Button(buttons, text=self.t('exp_close'), command=window.destroy).pack(side='right')
        ttk.Button(buttons, text=self.t('exp_previous'), command=lambda: self.exp_reset('previous')).pack(side='left')
        ttk.Button(buttons, text=self.t('exp_defaults'), command=lambda: self.exp_reset('defaults')).pack(
            side='left', padx=8)
        ttk.Button(buttons, text=self.t('ps_export'), command=self.exp_export).pack(side='left')
        ttk.Button(buttons, text=self.t('ps_import'), command=self.exp_import).pack(side='left', padx=8)

    def exp_tags(self, item):
        tags = [self.t('exp_live') if item.live else self.t('exp_restart')]
        if backports_table.is_recommended(item):
            tags.append(self.t('exp_recommended'))
        if item.risky:
            tags.append(self.t('exp_experimental'))
        if item.broken:
            tags.append(self.t('exp_broken'))
        return tags

    def exp_row(self, parent, item):
        row = ttk.Frame(parent)
        row.pack(fill='x', pady=3)
        row.columnconfigure(0, weight=1)
        name = self.exp_name(item.name)
        label = f"{item.id} · {name}" if item.group == 'bp' else name
        current = self.exp_states.get(item.id)
        value = backports_table.effective(item, self.exp_states)
        if item.vtype == 'bool' and item.default is not None:
            var = tk.BooleanVar(value=value == '1')
            ttk.Checkbutton(row, text=label, variable=var,
                            command=lambda: self.exp_set(item, '1' if var.get() else '0')).grid(
                row=0, column=0, sticky='w')
        else:
            ttk.Label(row, text=label).grid(row=0, column=0, sticky='w')
            if item.vtype == 'float':
                fvar = tk.DoubleVar(value=float(value))

                def fcommit(_=None, item=item, var=fvar):
                    try:
                        self.exp_set(item, str(var.get()))
                    except tk.TclError:
                        pass
                fspin = ttk.Spinbox(row, from_=item.lo, to=item.hi, textvariable=fvar, width=10,
                                    increment=backports_table.FLOAT_STEPS.get(item.key, 0.05), command=fcommit)
                fspin.grid(row=0, column=1, padx=6)
                fspin.bind('<FocusOut>', fcommit)
                fspin.bind('<Return>', fcommit)
                ttk.Label(row, text=f'{item.unit} ({self.t("exp_default")}: {item.default}{item.unit})',
                          foreground='#777').grid(row=0, column=2, sticky='w')
            elif item.vtype == 'int':
                var = tk.IntVar(value=int(value))

                def commit(_=None, item=item, var=var):
                    try:
                        self.exp_set(item, str(var.get()))
                    except tk.TclError:
                        pass
                spin = ttk.Spinbox(row, from_=item.lo, to=item.hi, textvariable=var, width=7, command=commit)
                spin.grid(row=0, column=1, padx=6)
                spin.bind('<FocusOut>', commit)
                spin.bind('<Return>', commit)
                ttk.Label(row, text=f'{item.unit} ({self.t("exp_default")}: {item.default}{item.unit})',
                          foreground='#777').grid(row=0, column=2, sticky='w')
            else:
                if item.vtype == 'bool':  # automatic
                    options = [(None, self.t('exp_auto')), ('1', self.t('exp_on')), ('0', self.t('exp_off'))]
                else:
                    options = [(v, (pt if self.pt() else en) + (f' ({self.t("exp_default")})' if v == item.default else ''))
                               for v, en, pt in item.choices]
                box = ttk.Combobox(row, values=[o[1] for o in options], state='readonly', width=26)
                box.current(next((i for i, o in enumerate(options) if o[0] == value), 0))
                box.grid(row=0, column=1, padx=6)
                box.bind('<<ComboboxSelected>>', lambda _, o=options, b=box, i=item: self.exp_set(i, o[b.current()][0]))
        tags = ' · '.join(self.exp_tags(item))
        ttk.Label(row, text=f'[{tags}]', foreground='#b36b00' if item.risky or item.broken else '#2a6fdb').grid(
            row=1, column=0, columnspan=3, sticky='w')
        what = self.exp_name(item.what)
        if item.source:
            what += ' (' + self.t('exp_source', source=item.source) + ')'
        ttk.Label(row, text=what, wraplength=760, justify='left', foreground='#555').grid(
            row=2, column=0, columnspan=3, sticky='w')
        if item.shader:
            ttk.Label(row, text='⚠ ' + self.t('exp_shader'), wraplength=760, justify='left',
                      foreground='#b36b00').grid(row=3, column=0, columnspan=3, sticky='w')

    def append(self, line):
        self.log.configure(state='normal')
        self.log.insert('end', line if line.endswith('\n') else line + '\n')
        if int(self.log.index('end-1c').split('.')[0]) > 2000:
            self.log.delete('1.0', '500.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def play(self):
        if self.process or self.starting:
            messagebox.showinfo(self.t('title'), self.t('already'))
            return
        self.starting = True
        self.changed()
        self.append(self.t('preparing'))
        self.play_button.configure(state='disabled')
        self.cache_button.configure(state='disabled')
        self.precompiler = precompile.Precompiler()
        threading.Thread(target=self.run_game, daemon=True).start()

    def jobs_progress(self, phase, n, total):
        """A line in the log every 25% of a generator phase (the log has no progress bar)."""
        step = (4 * n // total) if total else 0
        if step > self._jobs_step.get(phase, 0):
            self._jobs_step[phase] = step
            if n < total:
                self.lines.put(f'  {precompile.message(precompile.PHASE_TEXT.get(phase, "jobs_phase_other"), self.settings, phase=phase)} {100 * n // total}%')

    def run_game(self):
        try:
            tlg_run.prepare(self.settings, log=lambda line: self.lines.put(line))
            self._jobs_step = {}
            outcome = precompile.prepare_jobs(self.settings, log=lambda line: self.lines.put(line),
                                              progress=self.jobs_progress, precompiler=self.precompiler)
            if outcome == 'cancelled':   # window closed in the middle: do not start the game
                self.lines.put(self.t('jobs_cancelled'))
                self.lines.put(('stopped', None))
                return
            self.process, log = tlg_run.launch(self.settings)
        except tlg_run.GameError as error:  # a problem with the game files, in the launcher's language
            text = self.t(error.key, detail=error.detail)
            self.lines.put(text)
            self.lines.put(('error', text))
            return
        except Exception as error:  # shown to the user, the launcher keeps running
            self.lines.put(str(error))
            self.lines.put(('error', str(error)))
            return
        self.lines.put(f"{self.t('running')} {log}")
        self.lines.put(('started', log))
        code = self.process.wait()   # the game writes its own log file; here only the exit code
        self.process = None
        self.lines.put(('ended', code))

    def pump(self):
        try:
            while True:
                item = self.lines.get_nowait()
                if isinstance(item, tuple) and item[0] == 'started':
                    self.start_tail(item[1])
                elif isinstance(item, tuple):
                    kind, value = item
                    self.starting = False
                    self.stop_tail()
                    if kind == 'stopped':
                        pass
                    elif kind == 'error':
                        messagebox.showerror(self.t('error'), value)
                    else:
                        message = self.t('ended' if value in (0, -15) else 'crashed', code=value)
                        self.append(message)
                        if value not in (0, -15):
                            messagebox.showwarning(self.t('title'), message)
                    self.refresh_status()
                else:
                    self.append(item)
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def start_tail(self, log):
        """Follow the game's log file once a second; optionally close the launcher soon after."""
        self.tail = logtail.LogTail(log, LOG_LINES)
        self.root.after(LOG_POLL_MS, self.tail_tick)
        if self.settings.get('close_on_play'):
            self.root.after(CLOSE_DELAY_MS, self.quit)

    def stop_tail(self):
        if self.tail is not None:
            self.show_lines(self.tail.poll(final=True))
            self.tail = None

    def tail_tick(self):
        if self.tail is not None:
            self.show_lines(self.tail.poll())
            self.root.after(LOG_POLL_MS, self.tail_tick)

    def show_lines(self, lines):
        """New log lines in one go: a single insert and a single scroll per tick."""
        if not lines:
            return
        self.log.configure(state='normal')
        self.log.insert('end', '\n'.join(lines) + '\n')
        extra = int(self.log.index('end-1c').split('.')[0]) - LOG_LINES
        if extra > 0:
            self.log.delete('1.0', f'{extra + 1}.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def quit(self):
        # The game is an independent process: closing the launcher does not close it.
        self.precompiler.cancel()
        self.root.destroy()


def main():
    # Packaged executable roles: --script <file> runs a preparation script, --play starts the
    # game with the saved settings and no launcher window (shortcuts, Steam).
    multiprocessing.freeze_support()   # packaged: the shader list generator starts worker processes (re-runs this exe)
    args = sys.argv[1:]
    if args[:1] == ['--script'] and len(args) > 1:
        sys.exit(tlg_run.run_script(args[1:]))
    if args[:1] == ['--clear-shader-cache']:
        sys.exit(tlg_run.main(args))
    if args[:1] == ['--play']:
        sys.exit(tlg_run.main(args[1:]))
    root = tk.Tk(className='infamous-pc')  # matches StartupWMClass in the desktop entry
    try:
        ttk.Style().theme_use('clam')
    except tk.TclError:
        pass
    Launcher(root)
    root.mainloop()


if __name__ == '__main__':
    main()
