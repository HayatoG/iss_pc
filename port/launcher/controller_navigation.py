# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher-only SDL3 navigation. Uses the bundled runtime's controller mappings."""
import ctypes as C
import time
import os
from pathlib import Path
from PySide6.QtCore import QObject,Signal,Slot,Property,QRectF,QPointF,QTimer,QCoreApplication,QEvent,Qt,QMetaObject,Q_ARG
from PySide6.QtGui import QKeyEvent

class NavigationState:
    def __init__(self):self.previous=set();self.direction=None;self.next_repeat=0.
    def update(self,buttons,x,y,now,active=True):
        buttons=set(buttons)
        direction=next((name for i,name in [(11,'up'),(12,'down'),(13,'left'),(14,'right')] if i in buttons),None)
        if direction is None and max(abs(x),abs(y))>=15000:
            direction=('right' if x>0 else 'left') if abs(x)>abs(y) else ('down' if y>0 else 'up')
        if not active:self.previous=buttons;self.direction=direction;self.next_repeat=now+.35;return []
        result=[]
        for button,action in [(9,'previous_tab'),(10,'next_tab'),(0,'confirm'),(1,'back'),(6,'options')]:
            if button in buttons and button not in self.previous and not(button in (9,10) and 9 in buttons and 10 in buttons):result.append(action)
        if direction is not None:
            if direction!=self.direction:result.append(direction);self.next_repeat=now+.35
            elif now>=self.next_repeat:result.append(direction);self.next_repeat=now+.12
        self.previous=buttons;self.direction=direction
        return result

class ControllerNavigation(QObject):
    action=Signal(str)
    scroll=Signal(float)
    boundary=Signal(str)
    focusChanged=Signal()
    def __init__(self,app):
        super().__init__(app.qt);self.app=app;self.state=NavigationState();self.pad=None;self.sdl=None;self.next_scan=0.;self.initialized=False;self.error=''
        self.timer=QTimer(self);self.timer.setInterval(20);self.timer.timeout.connect(self.poll)
        self.edit_control=None;self._focus_rect=QRectF()
        self.last_poll=time.monotonic()
        self.focus_timer=QTimer(self);self.focus_timer.setInterval(40);self.focus_timer.timeout.connect(self.updateFocus);self.focus_timer.start()
        app.qt.aboutToQuit.connect(self.close)
        try:
            import infamous_run as run
            dll_dir=Path(run.PORT_DIR)/'out'
            self.dll_directory=os.add_dll_directory(str(dll_dir)) if hasattr(os,'add_dll_directory') else None
            self.sdl=C.CDLL(str(dll_dir/'SDL3.dll'))
            def bind(name,args,returns):
                fn=getattr(self.sdl,name);fn.argtypes=args;fn.restype=returns;return fn
            bind('SDL_SetHint',[C.c_char_p,C.c_char_p],C.c_bool)
            bind('SDL_SetMainReady',[],None);bind('SDL_GetError',[],C.c_char_p)
            bind('SDL_InitSubSystem',[C.c_uint32],C.c_bool);bind('SDL_QuitSubSystem',[C.c_uint32],None)
            bind('SDL_PumpEvents',[],None);bind('SDL_UpdateGamepads',[],None)
            bind('SDL_SetGamepadEventsEnabled',[C.c_bool],None);bind('SDL_SetJoystickEventsEnabled',[C.c_bool],None)
            bind('SDL_GetGamepads',[C.POINTER(C.c_int)],C.POINTER(C.c_uint32));bind('SDL_free',[C.c_void_p],None)
            bind('SDL_OpenGamepad',[C.c_uint32],C.c_void_p);bind('SDL_CloseGamepad',[C.c_void_p],None)
            bind('SDL_GamepadConnected',[C.c_void_p],C.c_bool);bind('SDL_GetGamepadButton',[C.c_void_p,C.c_int],C.c_bool)
            bind('SDL_GetGamepadAxis',[C.c_void_p,C.c_int],C.c_int16)
            # SDL has no window here; Qt's foreground/game-state checks gate all actions.
            self.sdl.SDL_SetHint(b'SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS',b'1')
            self.sdl.SDL_SetMainReady()
            self.initialized=bool(self.sdl.SDL_InitSubSystem(0x00002000))
            if not self.initialized:raise RuntimeError(self.sdl.SDL_GetError().decode('utf-8','replace'))
            if self.initialized:
                self.sdl.SDL_SetGamepadEventsEnabled(False);self.sdl.SDL_SetJoystickEventsEnabled(False);self.timer.start()
        except Exception as error:self.error=str(error);print('Controller navigation unavailable:',error,flush=True);self.close()
    def poll(self):
        if not self.initialized:return
        try:
            self.sdl.SDL_PumpEvents();self.sdl.SDL_UpdateGamepads();now=time.monotonic()
            if self.pad and not self.sdl.SDL_GamepadConnected(self.pad):self.sdl.SDL_CloseGamepad(self.pad);self.pad=None;self.state=NavigationState()
            if not self.pad and now>=self.next_scan:
                self.next_scan=now+1.;count=C.c_int();ids=self.sdl.SDL_GetGamepads(C.byref(count))
                try:
                    if ids:
                        for i in range(count.value):
                            self.pad=self.sdl.SDL_OpenGamepad(ids[i])
                            if self.pad:break
                finally:
                    if ids:self.sdl.SDL_free(ids)
            if not self.pad:return
            buttons={i for i in [0,1,6,9,10,11,12,13,14] if self.sdl.SDL_GetGamepadButton(self.pad,i)}
            active=self.app.window.isActive() and self.app.backend.property('playState')=='idle'
            for action in self.state.update(buttons,self.sdl.SDL_GetGamepadAxis(self.pad,0),self.sdl.SDL_GetGamepadAxis(self.pad,1),now,active):self.action.emit(action)
            delta=self.scrollDelta(self.sdl.SDL_GetGamepadAxis(self.pad,3),now-self.last_poll,active and not self.editing)
            self.last_poll=now
            if delta:self.scroll.emit(delta)
        except Exception as error:print('Controller navigation stopped:',error,flush=True);self.close()
    @Slot(str)
    def sendKey(self,action):
        key={'up':Qt.Key_Up,'down':Qt.Key_Down,'left':Qt.Key_Left,'right':Qt.Key_Right,'confirm':Qt.Key_Return,'back':Qt.Key_Escape,'tab':Qt.Key_Tab}.get(action)
        if key is None:return
        event=QKeyEvent(QEvent.KeyPress,key,Qt.NoModifier);event.setAccepted(False)
        QCoreApplication.sendEvent(self.app.window,event);QCoreApplication.sendEvent(self.app.window,QKeyEvent(QEvent.KeyRelease,key,Qt.NoModifier))
        if not event.isAccepted() and action in ('up','down','left','right','confirm'):
            fallback=Qt.Key_Space if action=='confirm' else Qt.Key_Backtab if action in ('up','left') else Qt.Key_Tab
            modifiers=Qt.ShiftModifier if fallback==Qt.Key_Backtab else Qt.NoModifier
            QCoreApplication.sendEvent(self.app.window,QKeyEvent(QEvent.KeyPress,fallback,modifiers))
            QCoreApplication.sendEvent(self.app.window,QKeyEvent(QEvent.KeyRelease,fallback,modifiers))
    @Slot(QObject)
    def focusContent(self,root):
        items=self.candidates(root)
        if items:self.focus(items[0])
    def candidates(self,root):
        result=[]
        def visit(item):
            if not hasattr(item,'isVisible') or not item.isVisible() or not item.isEnabled():return
            if item.property('controllerActive') is False:return
            if item.width()>0 and item.height()>0 and (item.property('controllerFocusable') or item.property('activeFocusOnTab')):
                result.append(item);return
            for child in item.childItems():visit(child)
        if root:visit(root)
        return result
    def focus(self,item):
        item.forceActiveFocus(Qt.TabFocusReason)
        self.ensureVisible(item);self.updateFocus()
    def ensureVisible(self,item):
        control=item;item=self.frame(item);parent=item.parentItem()
        while parent:
            if parent.property('contentY') is not None and parent.property('contentHeight') is not None:
                p=item.mapToItem(parent,QPointF(0,0));y=float(parent.property('contentY'))
                if hasattr(parent,'cancelFlick'):parent.cancelFlick()
                if item.height()>parent.height()-32 or p.y()<16:y+=p.y()-16
                elif p.y()+item.height()>parent.height()-16:y+=p.y()+item.height()-parent.height()+16
                low,high=self.scrollBounds(parent)
                peers=[i for i in self.candidates(parent) if self.lane(i)==parent]
                if peers:
                    first=min(i.mapToItem(parent,QPointF(0,i.height()/2)).y() for i in peers)
                    cy=control.mapToItem(parent,QPointF(0,control.height()/2)).y()
                    # The first action row restores its title/intro, not only the button.
                    if cy<=first+12 and p.y()+item.height()+float(parent.property('contentY'))-low<=parent.height()-16:y=low
                parent.setProperty('contentY',max(low,min(y,high)))
            parent=parent.parentItem()
    def scrollBounds(self,view):
        low=float(view.property('originY') or 0)
        header=view.property('headerItem')
        if hasattr(header,'toQObject'):header=header.toQObject()
        if header is not None:low=min(low,header.y())
        return low,max(low,low+float(view.property('contentHeight'))-view.height())
    @staticmethod
    def scrollDelta(axis,elapsed,active=True):
        if not active or abs(axis)<=11000:return 0.
        amount=min(1.,(abs(axis)-11000)/21767.)**1.4
        return (1 if axis>0 else -1)*amount*1050*min(.05,max(0.,elapsed))
    @Slot(QObject,float)
    def scrollPage(self,root,delta):
        if self.editing or not root:return
        focus=self.app.window.activeFocusItem();view=self.lane(focus) if focus else None
        views=[]
        def collect(item):
            if not item.isVisible() or not item.isEnabled() or item.property('controllerActive') is False:return
            if item.property('contentY') is not None and float(item.property('contentHeight'))>item.height()+1:views.append(item)
            for child in item.childItems():collect(child)
        collect(root)
        if view not in views:view=max(views,key=lambda i:i.width()*i.height(),default=None)
        if view:
            low,high=self.scrollBounds(view)
            QMetaObject.invokeMethod(view,'cancelFlick',Qt.DirectConnection)
            view.setProperty('contentY',max(low,min(float(view.property('contentY'))+delta,high)))
            self.updateFocus()
    def frame(self,item):
        target=item.property('controllerFocusTarget')
        if hasattr(target,'toQObject'):target=target.toQObject()
        return target if target is not None else item
    def lane(self,item):
        parent=item.parentItem()
        while parent:
            if parent.property('contentY') is not None:return parent
            parent=parent.parentItem()
        return None
    @Slot()
    def resetFocus(self):self._focus_rect=QRectF();self.focusChanged.emit()
    @Property(QRectF,notify=focusChanged)
    def focusRect(self):return self._focus_rect
    @Property(bool,notify=focusChanged)
    def editing(self):
        try:return bool(self.edit_control and self.edit_control.isVisible() and self.edit_control.property('controllerEditing'))
        except RuntimeError:return False
    @Slot()
    def updateFocus(self):
        try:
            item=self.app.window.activeFocusItem();rect=QRectF()
            while item and not(item.property('controllerFocusable') or item.property('activeFocusOnTab')):item=item.parentItem()
            if item and item.isVisible() and not item.property('controllerOwnFocus'):
                item=self.frame(item)
                p=item.mapToItem(self.app.window.contentItem(),QPointF(0,0));rect=QRectF(p.x(),p.y(),item.width(),item.height())
                parent=item.parentItem()
                while parent:
                    if parent.clip():
                        p=parent.mapToItem(self.app.window.contentItem(),QPointF(0,0))
                        rect=rect.intersected(QRectF(p.x(),p.y(),parent.width(),parent.height()))
                    parent=parent.parentItem()
            if rect!=self._focus_rect:self._focus_rect=rect;self.focusChanged.emit()
        except RuntimeError:self._focus_rect=QRectF();self.focusChanged.emit()
    @Slot(QObject,str)
    def navigate(self,root,action):
        if self.edit_control:
            try:
                if self.edit_control.property('controllerEditing'):
                    if self.edit_control.isVisible() and self.edit_control.property('activeFocus'):
                        QMetaObject.invokeMethod(self.edit_control,'controllerInput',Qt.DirectConnection,Q_ARG('QVariant',action));self.focusChanged.emit();return
                    QMetaObject.invokeMethod(self.edit_control,'controllerInput',Qt.DirectConnection,Q_ARG('QVariant','back'))
            except RuntimeError:pass
            self.edit_control=None
        items=self.candidates(root);focused=self.app.window.activeFocusItem();owner=None
        while focused:
            if focused in items:owner=focused;break
            focused=focused.parentItem()
        if action=='back':self.sendKey('back');QTimer.singleShot(0,self.updateFocus);return
        if not owner:
            if items:self.focus(items[0])
            return
        # A manually scrolled-away setting must never activate offscreen.
        if action=='confirm' and self.lane(owner):
            view=self.lane(owner);point=self.frame(owner).mapToItem(view,QPointF(0,0))
            if point.y()+self.frame(owner).height()<0 or point.y()>view.height():
                visible=[i for i in items if self.lane(i)==view and 0<=i.mapToItem(view,QPointF(0,i.height()/2)).y()<=view.height()]
                if visible:self.focus(visible[0])
                return
        if action=='confirm':
            if owner.property('controllerKind') in ('choice','slider'):
                self.edit_control=owner
                QMetaObject.invokeMethod(owner,'controllerInput',Qt.DirectConnection,Q_ARG('QVariant',action))
                self.focusChanged.emit()
            else:self.sendKey('confirm')
            QTimer.singleShot(0,self.updateFocus);return
        if action not in ('up','down','left','right'):return
        origin=owner.mapToItem(root,QPointF(owner.width()/2,owner.height()/2));scored=[];lane=self.lane(owner)
        for other in items:
            if other==owner:continue
            if action in ('up','down') and self.lane(other)!=lane:continue
            p=other.mapToItem(root,QPointF(other.width()/2,other.height()/2));dx=p.x()-origin.x();dy=p.y()-origin.y()
            primary={'up':-dy,'down':dy,'left':-dx,'right':dx}[action];secondary=abs(dx if action in ('up','down') else dy)
            if primary>4:scored.append(((primary,secondary) if action in ('up','down') else (secondary*3+primary,primary),other))
        if scored:self.focus(min(scored,key=lambda row:row[0])[1])
        elif action in ('up','down'):
            if action=='up' and lane is not None:
                headers=[i for i in items if i.property('controllerHeader')]
                if headers:self.focus(min(headers,key=lambda i:abs(i.mapToItem(root,QPointF(i.width()/2,0)).x()-origin.x())));return
            if lane is None:
                other_items=[i for i in items if self.lane(i) is not None]
                possible=[]
                for other in other_items:
                    p=other.mapToItem(root,QPointF(other.width()/2,other.height()/2));dy=(p.y()-origin.y())*(1 if action=='down' else -1)
                    if dy>4:possible.append((dy,other))
                if possible:self.focus(min(possible,key=lambda row:row[0])[1]);return
            self.boundary.emit(action)
    @Slot()
    def close(self):
        self.timer.stop()
        self.focus_timer.stop()
        if self.pad:self.sdl.SDL_CloseGamepad(self.pad);self.pad=None
        if self.initialized:self.sdl.SDL_QuitSubSystem(0x00002000);self.initialized=False

def attach(app):
    app.controller_navigation=ControllerNavigation(app)
    app.engine.rootContext().setContextProperty('ControllerNav',app.controller_navigation)
