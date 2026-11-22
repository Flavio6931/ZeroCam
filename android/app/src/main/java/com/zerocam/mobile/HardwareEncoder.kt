package com.zerocam.mobile

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.view.Surface
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Codificador H.264 por hardware mediante MediaCodec con Surface de entrada zero-copy.
 */
class HardwareEncoder(
    private val width: Int,
    private val height: Int,
    private val fps: Int,
    private val bitrateBps: Int,
    private val onEncodedFrame: (payload: ByteArray, ptsUs: Long, isKeyframe: Boolean, hasConfig: Boolean) -> Unit,
    private val onError: (String) -> Unit
) {
    private val running = AtomicBoolean(false)
    private val stateLock = Any()
    @Volatile private var codec: MediaCodec? = null
    private var drainThread: Thread? = null

    @Volatile var inputSurface: Surface? = null
        private set

    @Volatile private var spsPpsConfig: ByteArray? = null

    /**
     * Inicializa el codificador AVC en modo Surface de baja latencia y arranca el hilo drenador.
     */
    fun start(): Boolean {
        return try {
            val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, bitrateBps)
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                if (Build.VERSION.SDK_INT >= 29) {
                    setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
                }
                if (Build.VERSION.SDK_INT >= 30) {
                    setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
                }
                setInteger(MediaFormat.KEY_PRIORITY, 0)
            }

            val mc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            synchronized(stateLock) {
                // Asigna inmediatamente para garantizar release() en stop() ante cualquier fallo posterior.
                codec = mc
            }
            try {
                format.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
                mc.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            } catch (_: Exception) {
                mc.reset()
                format.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
                mc.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            }

            val surface = mc.createInputSurface()
            synchronized(stateLock) {
                inputSurface = surface
            }
            mc.start()
            running.set(true)

            val t = Thread({ drainLoop(mc) }, "ZeroCamHwEncoder")
            synchronized(stateLock) {
                drainThread = t
            }
            t.start()
            true
        } catch (e: Exception) {
            stop()
            onError("Error iniciando MediaCodec H.264: ${e.message}")
            false
        }
    }

    /**
     * Solicita inmediatamente un frame IDR (útil al conectar o reconectar el cliente TCP).
     */
    fun requestKeyFrame() {
        synchronized(stateLock) {
            if (!running.get()) return
            try {
                val bundle = Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                }
                codec?.setParameters(bundle)
            } catch (_: Exception) {
            }
        }
    }

    /**
     * Ajusta el bitrate de salida en caliente sin reiniciar MediaCodec ni cortar la cámara.
     */
    fun updateBitrate(newBitrateBps: Int): Boolean {
        synchronized(stateLock) {
            if (!running.get()) return false
            val mc = codec ?: return false
            return try {
                val bundle = Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, newBitrateBps)
                }
                mc.setParameters(bundle)
                val syncBundle = Bundle().apply {
                    putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0)
                }
                mc.setParameters(syncBundle)
                true
            } catch (_: Exception) {
                false
            }
        }
    }

    /**
     * Detiene el hilo drenador y libera MediaCodec y su Surface de entrada de forma sincronizada.
     */
    fun stop() {
        val threadToJoin: Thread?
        synchronized(stateLock) {
            running.set(false)
            try { codec?.signalEndOfInputStream() } catch (_: Exception) {}
            threadToJoin = drainThread
            drainThread = null
        }
        if (threadToJoin != null && threadToJoin !== Thread.currentThread()) {
            try { threadToJoin.join(1000) } catch (_: InterruptedException) {}
        }
        synchronized(stateLock) {
            try { codec?.stop() } catch (_: Exception) {}
            try { codec?.release() } catch (_: Exception) {}
            codec = null
            try { inputSurface?.release() } catch (_: Exception) {}
            inputSurface = null
            spsPpsConfig = null
        }
    }

    private fun drainLoop(mc: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running.get()) {
            try {
                val idx = mc.dequeueOutputBuffer(info, 15_000L)
                if (idx >= 0) {
                    val buf = mc.getOutputBuffer(idx)
                    if (buf != null && info.size > 0) {
                        buf.position(info.offset)
                        buf.limit(info.offset + info.size)
                        val chunk = ByteArray(info.size)
                        buf.get(chunk)

                        val isConfig = (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) != 0
                        val isKeyframe = (info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME) != 0

                        if (isConfig) {
                            spsPpsConfig = chunk
                        } else {
                            val cfg = spsPpsConfig
                            if (isKeyframe && cfg != null) {
                                val combined = ByteArray(cfg.size + chunk.size)
                                System.arraycopy(cfg, 0, combined, 0, cfg.size)
                                System.arraycopy(chunk, 0, combined, cfg.size, chunk.size)
                                onEncodedFrame(combined, info.presentationTimeUs, true, true)
                            } else {
                                onEncodedFrame(chunk, info.presentationTimeUs, isKeyframe, false)
                            }
                        }
                    }
                    mc.releaseOutputBuffer(idx, false)
                }
            } catch (_: Exception) {
                if (running.get()) break
            }
        }
    }
}
