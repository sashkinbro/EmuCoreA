package com.sbro.emucorea.data

import android.content.res.Resources
import android.graphics.BitmapFactory
import android.graphics.ImageDecoder
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.os.Build
import java.io.File
import kotlin.math.max

/** Bounds decoded images on every supported Android version. */
internal fun decodeHomeBackgroundImage(resources: Resources, file: File, maxDimension: Int): Drawable {
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
        return ImageDecoder.decodeDrawable(ImageDecoder.createSource(file)) { decoder, info, _ ->
            var sampleSize = 1
            while (max(info.size.width, info.size.height) / sampleSize > maxDimension) sampleSize *= 2
            decoder.setTargetSampleSize(sampleSize)
        }
    }
    // Android 8 has no AnimatedImageDrawable; show a sampled first GIF frame.
    val options = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeFile(file.absolutePath, options)
    require(options.outWidth > 0 && options.outHeight > 0) { "Selected image cannot be decoded" }
    options.inSampleSize = 1
    while (max(options.outWidth, options.outHeight) / options.inSampleSize > maxDimension) {
        options.inSampleSize *= 2
    }
    options.inJustDecodeBounds = false
    val bitmap = requireNotNull(BitmapFactory.decodeFile(file.absolutePath, options)) {
        "Selected image cannot be decoded"
    }
    return BitmapDrawable(resources, bitmap)
}
