/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Code Aurora Forum, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

package android.webkit;

import android.graphics.SurfaceTexture;
import android.media.MediaPlayer;
import android.net.Uri;
import android.util.Log;
import android.view.SurfaceView;
import android.webkit.HTML5VideoViewProxy;
import java.io.IOException;
import java.util.HashMap;
import java.util.Map;
import java.util.Timer;
import java.util.TimerTask;

/**
 * @hide This is only used by the browser
 */
public class HTML5VideoView implements MediaPlayer.OnPreparedListener {

    protected static final String LOGTAG = "HTML5VideoView";

    protected static final String COOKIE = "Cookie";
    protected static final String HIDE_URL_LOGS = "x-hide-urls-from-log";

    // For handling the seekTo before prepared, we need to know whether or not
    // the video is prepared. Therefore, we differentiate the state between
    // prepared and not prepared.
    // When the video is not prepared, we will have to save the seekTo time,
    // and use it when prepared to play.
    // NOTE: these values are in sync with VideoLayerAndroid.h in webkit side.
    // Please keep them in sync when changed.
    static final int STATE_INITIALIZED        = 0;
    static final int STATE_NOTPREPARED        = 1;
    static final int STATE_PREPARED           = 2;
    static final int STATE_PLAYING            = 3;
    static final int STATE_BUFFERING          = 4;
    static final int STATE_RELEASED           = 5;
    protected int mCurrentState;

    protected HTML5VideoViewProxy mProxy;

    // Save the seek time when not prepared. This can happen when switching
    // video besides initial load.
    protected int mSaveSeekTime;

    // This is used to find the VideoLayer on the native side.
    protected int mVideoLayerId;

    // Every video will have one MediaPlayer. Given the fact we only have one
    // SurfaceTexture, there is only one MediaPlayer in action. Every time we
    // switch videos, a new instance of MediaPlayer will be created in reset().
    // Switching between inline and full screen will also create a new instance.
    protected MediaPlayer mPlayer;

    // This will be set up every time we create the Video View object.
    // Set to true only when switching into full screen while playing
    protected boolean mAutostart;

    // We need to save such info.
    protected Uri mUri;
    protected Map<String, String> mHeaders;

    // The timer for timeupate events.
    // See http://www.whatwg.org/specs/web-apps/current-work/#event-media-timeupdate
    protected Timer mTimer;

    // The spec says the timer should fire every 250 ms or less.
    private static final int TIMEUPDATE_PERIOD = 250;  // ms

    protected boolean mPauseDuringPreparing;
    // common Video control FUNCTIONS:
    public void start() {
        if (mCurrentState == STATE_PREPARED) {
            // When replaying the same video, there is no onPrepared call.
            // Therefore, the timer should be set up here.
            if (mTimer == null)
            {
                mTimer = new Timer();
                mTimer.schedule(new TimeupdateTask(mProxy), TIMEUPDATE_PERIOD,
                        TIMEUPDATE_PERIOD);
            }
            mPlayer.start();
            setPlayerBuffering(false);
        }
    }

    public void pause() {
        if (isPlaying()) {
            mPlayer.pause();
        } else if (mCurrentState == STATE_NOTPREPARED) {
            mPauseDuringPreparing = true;
        }
        // Delete the Timer to stop it since there is no stop call.
        if (mTimer != null) {
            mTimer.purge();
            mTimer.cancel();
            mTimer = null;
        }
    }

    public int getDuration() {
        if (mCurrentState == STATE_PREPARED) {
            return mPlayer.getDuration();
        } else {
            return -1;
        }
    }

    public int getCurrentPosition() {
        if (mCurrentState == STATE_PREPARED) {
            return mPlayer.getCurrentPosition();
        }
        return 0;
    }

    public void seekTo(int pos) {
        if (mCurrentState == STATE_PREPARED)
            mPlayer.seekTo(pos);
        else
            mSaveSeekTime = pos;
    }

    public boolean isPlaying() {
        if (mCurrentState == STATE_PREPARED) {
            return mPlayer.isPlaying();
        } else {
            return false;
        }
    }

    public void release() {
        if (mCurrentState != STATE_RELEASED) {
            stopPlayback();
            mPlayer.release();
        }
        mCurrentState = STATE_RELEASED;
    }

    public void stopPlayback() {
        if (mCurrentState == STATE_PREPARED) {
            mPlayer.stop();
        }
    }

    public boolean getAutostart() {
        return mAutostart;
    }

    public boolean getPauseDuringPreparing() {
        return mPauseDuringPreparing;
    }

    public void setVolume(float volume) {
        if (mCurrentState == STATE_PREPARED) {
            mPlayer.setVolume(volume, volume);
        }
    }

    // Every time we start a new Video, we create a VideoView and a MediaPlayer
    public void init(int videoLayerId, int position, boolean autoStart) {
        mPlayer = new MediaPlayer();
        mCurrentState = STATE_INITIALIZED;
        mProxy = null;
        mVideoLayerId = videoLayerId;
        mSaveSeekTime = position;
        mAutostart = autoStart;
        mTimer = null;
        mPauseDuringPreparing = false;
    }

    protected HTML5VideoView() {
    }

    protected static Map<String, String> generateHeaders(String url,
            HTML5VideoViewProxy proxy) {
        boolean isPrivate = proxy.getWebView().isPrivateBrowsingEnabled();
        String cookieValue = CookieManager.getInstance().getCookie(url, isPrivate);
        Map<String, String> headers = new HashMap<String, String>();
        if (cookieValue != null) {
            headers.put(COOKIE, cookieValue);
        }
        if (isPrivate) {
            headers.put(HIDE_URL_LOGS, "true");
        }

        return headers;
    }

    public void setVideoURI(String uri, HTML5VideoViewProxy proxy) {
        // When switching players, surface texture will be reused.
        mUri = Uri.parse(uri);
        mHeaders = generateHeaders(uri, proxy);
    }

    // Listeners setup FUNCTIONS:
    public void setOnCompletionListener(HTML5VideoViewProxy proxy) {
        mPlayer.setOnCompletionListener(proxy);
    }

    public void setOnErrorListener(HTML5VideoViewProxy proxy) {
        mPlayer.setOnErrorListener(proxy);
    }

    public void setOnPreparedListener(HTML5VideoViewProxy proxy) {
        mProxy = proxy;
        mPlayer.setOnPreparedListener(this);
    }

    public void setOnInfoListener(HTML5VideoViewProxy proxy) {
        mPlayer.setOnInfoListener(proxy);
    }

    // Normally called immediately after setVideoURI. But for full screen,
    // this should be after surface holder created
    public void prepareDataAndDisplayMode(HTML5VideoViewProxy proxy) {
        // SurfaceTexture will be created lazily here for inline mode
        decideDisplayMode();

        setOnCompletionListener(proxy);
        setOnPreparedListener(proxy);
        setOnErrorListener(proxy);
        setOnInfoListener(proxy);
        // When there is exception, we could just bail out silently.
        // No Video will be played though. Write the stack for debug
        try {
            mPlayer.setDataSource(mProxy.getContext(), mUri, mHeaders);
            mPlayer.prepareAsync();
        } catch (IllegalArgumentException e) {
            e.printStackTrace();
        } catch (IllegalStateException e) {
            e.printStackTrace();
        } catch (IOException e) {
            e.printStackTrace();
        }
        mCurrentState = STATE_NOTPREPARED;
    }


    // Common code
    public int getVideoLayerId() {
        return mVideoLayerId;
    }


    public int getCurrentState() {
        if (isPlaying()) {
            return STATE_PLAYING;
        } else {
            return mCurrentState;
        }
    }

    private final class TimeupdateTask extends TimerTask {
        private HTML5VideoViewProxy mProxy;

        public TimeupdateTask(HTML5VideoViewProxy proxy) {
            mProxy = proxy;
        }

        @Override
        public void run() {
            mProxy.onTimeupdate();
        }
    }

    @Override
    public void onPrepared(MediaPlayer mp) {
        mCurrentState = STATE_PREPARED;
        seekTo(mSaveSeekTime);
        if (mProxy != null) {
            mProxy.onPrepared(mp);
        }
        if (mPauseDuringPreparing) {
            pauseAndDispatch(mProxy);
            mPauseDuringPreparing = false;
        }
    }

    // Pause the play and update the play/pause button
    public void pauseAndDispatch(HTML5VideoViewProxy proxy) {
        pause();
        if (proxy != null) {
            proxy.dispatchOnPaused();
        }
    }

    // Below are functions that are different implementation on inline and full-
    // screen mode. Some are specific to one type, but currently are called
    // directly from the proxy.
    public void enterFullScreenVideoState(int layerId,
            HTML5VideoViewProxy proxy, WebView webView) {
    }

    public boolean isFullScreenMode() {
        return false;
    }

    public void decideDisplayMode() {
    }

    public boolean getReadyToUseSurfTex() {
        return false;
    }

    public SurfaceTexture getSurfaceTexture(int videoLayerId) {
        return null;
    }

    public int getTextureName() {
        return 0;
    }

    // This is true only when the player is buffering and paused
    public boolean mPlayerBuffering = false;

    public boolean getPlayerBuffering() {
        return mPlayerBuffering;
    }

    public void setPlayerBuffering(boolean playerBuffering) {
        mPlayerBuffering = playerBuffering;
        switchProgressView(playerBuffering);
    }

    protected void switchProgressView(boolean playerBuffering) {
        // Only used in HTML5VideoFullScreen
    }

    public boolean fullScreenExited() {
        // Only meaningful for HTML5VideoFullScreen
        return false;
    }

}
