package com.example.videolearn.videoplay

import android.os.Bundle
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import com.example.videolearn.R
import java.io.File

class VideoPlayActiivty : AppCompatActivity() {
    lateinit var surfaceView: SurfaceView
    var surfaceHolder: SurfaceHolder? = null
    var videoPlayHelper: VideoPlayHelper? = null
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_videoplay)
        surfaceView = findViewById(R.id.video_sv)
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                surfaceHolder = holder

            }

            override fun surfaceChanged(
                holder: SurfaceHolder,
                format: Int,
                width: Int,
                height: Int
            ) {
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
            }

        })
    }

    fun play(v: View) {
        surfaceHolder?.also {
            videoPlayHelper = VideoPlayHelper(
                File(application.externalCacheDir, "video.mp4").toString(),
                it.surface
            ).apply {
                play()
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        videoPlayHelper?.release()
    }
}