// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::util::*,
    crate::{FONTS_ALIASED_CM, FONTS_MEDIUM_CM, FONTS_SMALL_CM},
    fidl_fuchsia_fonts::{self as fonts, Style2, Width},
    fidl_fuchsia_intl::LocaleId,
};

// Add new tests here so we don't overload component manager with requests (58150)
#[fasync::run_singlethreaded(test)]
async fn test_get_typeface() -> Result<(), Error> {
    let factory = ProviderFactory::new();
    test_basic(&factory).await?;
    test_aliases(&factory).await?;
    test_aliases_with_language_overrides(&factory).await?;
    test_aliases_with_style_overrides(&factory).await?;
    test_font_collections(&factory).await?;
    test_fallback(&factory).await?;
    test_fallback_group(&factory).await?;
    test_postscript_and_full_name(&factory).await?;
    test_postscript_name_ignores_full_name(&factory).await?;
    test_postscript_name_ignores_other_parameters(&factory).await?;

    Ok(())
}

async fn test_basic(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_SMALL_CM).await?;

    let default = get_typeface_info_basic(&font_provider, None)
        .await
        .context("Failed to load default font")?;
    let roboto = get_typeface_info_basic(&font_provider, Some("Roboto".to_string()))
        .await
        .context("Failed to load Roboto")?;
    let material_icons =
        get_typeface_info_basic(&font_provider, Some("Material Icons".to_string()))
            .await
            .context("Failed to load Material Icons")?;

    // Roboto should be returned by default.
    assert_buf_eq!(default, roboto);

    // Material Icons request should return a different font.
    assert_ne!(default.vmo_koid, material_icons.vmo_koid);
    assert_ne!(default.buffer_id, material_icons.buffer_id);

    Ok(())
}

async fn test_aliases(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_SMALL_CM).await?;

    // Both requests should return the same font.
    let materialicons = get_typeface_info_basic(&font_provider, Some("MaterialIcons".to_string()))
        .await
        .context("Failed to load MaterialIcons")?;
    let material_icons =
        get_typeface_info_basic(&font_provider, Some("Material Design Icons".to_string()))
            .await
            .context("Failed to load Material Icons")?;

    assert_buf_eq!(materialicons, material_icons);

    Ok(())
}

async fn test_aliases_with_language_overrides(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_ALIASED_CM).await?;

    let a = get_typeface_info(
        &font_provider,
        Some("Alpha Sans".to_string()),
        None,
        Some(vec!["he".to_string()]),
        None,
    )
    .await
    .context("Failed to load Alpha Sans, languages: he")?;

    let b = get_typeface_info_basic(&font_provider, Some("Alpha Sans Hebrew".to_string()))
        .await
        .context("Failed to load Alpha Sans Hebrew")?;

    assert_buf_eq!(a, b);

    Ok(())
}

async fn test_aliases_with_style_overrides(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_ALIASED_CM).await?;

    let a = get_typeface_info(
        &font_provider,
        Some("Alpha Sans".to_string()),
        Some(Style2 {
            slant: None,
            weight: None,
            width: Some(Width::Condensed),
            ..Default::default()
        }),
        None,
        None,
    )
    .await
    .context("Failed to load Alpha Sans")?;

    let b = get_typeface_info_basic(&font_provider, Some("Alpha Sans Condensed".to_string()))
        .await
        .context("Failed to load Alpha Sans Condensed")?;

    assert_buf_eq!(a, b);

    Ok(())
}

async fn test_font_collections(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_MEDIUM_CM).await?;

    // Request Japanese and Simplified Chinese versions of Noto Sans CJK. Both
    // fonts are part of the same TTC file, so font provider is expected to
    // return the same buffer with different font index values.
    let noto_sans_cjk_ja = get_typeface_info(
        &font_provider,
        Some("Noto Sans CJK JP".to_string()),
        None,
        Some(vec!["ja".to_string()]),
        None,
    )
    .await
    .context("Failed to load NotoSansCJK JP font")?;
    let noto_sans_cjk_sc = get_typeface_info(
        &font_provider,
        Some("Noto Sans CJK SC".to_string()),
        None,
        Some(vec!["zh-Hans".to_string()]),
        None,
    )
    .await
    .context("Failed to load NotoSansCJK SC font")?;

    assert_buf_eq!(noto_sans_cjk_ja, noto_sans_cjk_sc);

    assert_ne!(
        noto_sans_cjk_ja.index, noto_sans_cjk_sc.index,
        "noto_sans_cjk_ja.index != noto_sans_cjk_sc.index\n \
         noto_sans_cjk_ja.index: {:?}\n \
         noto_sans_cjk_sc.index: {:?}",
        noto_sans_cjk_ja, noto_sans_cjk_sc
    );
    Ok(())
}

async fn test_fallback(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_MEDIUM_CM).await?;

    let noto_sans_cjk_ja = get_typeface_info(
        &font_provider,
        Some("Noto Sans CJK JP".to_string()),
        None,
        Some(vec!["ja".to_string()]),
        None,
    )
    .await
    .context("Failed to load NotoSansCJK JP font")?;

    let noto_sans_cjk_ja_by_char = get_typeface_info(
        &font_provider,
        Some("Roboto".to_string()),
        None,
        Some(vec!["ja".to_string()]),
        Some(vec!['な', 'ナ']),
    )
    .await
    .context("Failed to load NotoSansCJK JP font")?;

    // Same font should be returned in both cases.
    assert_buf_eq!(noto_sans_cjk_ja, noto_sans_cjk_ja_by_char);

    Ok(())
}

// Verify that the fallback group of the requested font is taken into account for fallback.
async fn test_fallback_group(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_MEDIUM_CM).await?;

    let noto_serif_cjk_ja = get_typeface_info(
        &font_provider,
        Some("Noto Serif CJK JP".to_string()),
        None,
        Some(vec!["ja".to_string()]),
        None,
    )
    .await
    .context("Failed to load Noto Serif CJK font")?;

    let noto_serif_cjk_ja_by_char = get_typeface_info(
        &font_provider,
        Some("Roboto Slab".to_string()),
        None,
        Some(vec!["ja".to_string()]),
        Some(vec!['な']),
    )
    .await
    .context("Failed to load Noto Serif CJK font")?;

    // The query above requested Roboto Slab, so it's expected to return
    // Noto Serif CJK instead of Noto Sans CJK because Roboto Slab and
    // Noto Serif CJK are both in serif fallback group.
    assert_buf_eq!(noto_serif_cjk_ja, noto_serif_cjk_ja_by_char);

    Ok(())
}

/// Verify that querying by Postscript name or by full name works.
async fn test_postscript_and_full_name(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_MEDIUM_CM).await?;

    let postscript_name_request = fonts::TypefaceRequest {
        query: Some(fonts::TypefaceQuery {
            postscript_name: Some("NotoSansCJKjp-Regular".to_string()),
            ..Default::default()
        }),
        ..Default::default()
    };
    let postscript_name_response =
        get_typeface_info_detailed(&font_provider, postscript_name_request).await?;

    let full_name_request = fonts::TypefaceRequest {
        query: Some(fonts::TypefaceQuery {
            full_name: Some("Noto Sans CJK JP".to_string()),
            ..Default::default()
        }),
        ..Default::default()
    };
    let full_name_response = get_typeface_info_detailed(&font_provider, full_name_request).await?;

    assert_buf_eq!(postscript_name_response, full_name_response);

    Ok(())
}

/// Verify that if both Postscript name and full name are provided, Postscript name wins.
async fn test_postscript_name_ignores_full_name(factory: &ProviderFactory) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_MEDIUM_CM).await?;

    let postscript_name_request = fonts::TypefaceRequest {
        query: Some(fonts::TypefaceQuery {
            postscript_name: Some("NotoSansCJKjp-Regular".to_string()),
            ..Default::default()
        }),
        ..Default::default()
    };
    let postscript_name_response =
        get_typeface_info_detailed(&font_provider, postscript_name_request).await?;

    let contradictory_request = fonts::TypefaceRequest {
        query: Some(fonts::TypefaceQuery {
            postscript_name: Some("NotoSansCJKjp-Regular".to_string()),
            full_name: Some("Roboto Slab".to_string()),
            ..Default::default()
        }),
        ..Default::default()
    };
    let contradictory_response =
        get_typeface_info_detailed(&font_provider, contradictory_request).await?;

    assert_buf_eq!(postscript_name_response, contradictory_response);

    Ok(())
}

/// Verify that if Postscript name is provided, other parameters are ignored.
async fn test_postscript_name_ignores_other_parameters(
    factory: &ProviderFactory,
) -> Result<(), Error> {
    let font_provider = factory.get_provider(FONTS_MEDIUM_CM).await?;

    let postscript_name_request = fonts::TypefaceRequest {
        query: Some(fonts::TypefaceQuery {
            postscript_name: Some("Roboto-Regular".to_string()),
            ..Default::default()
        }),
        ..Default::default()
    };
    let postscript_name_response =
        get_typeface_info_detailed(&font_provider, postscript_name_request).await?;

    let contradictory_request = fonts::TypefaceRequest {
        query: Some(fonts::TypefaceQuery {
            postscript_name: Some("Roboto-Regular".to_string()),
            languages: Some(vec![LocaleId { id: "ja".to_string() }]),
            code_points: Some(vec!['な' as u32]),
            ..Default::default()
        }),
        ..Default::default()
    };
    let contradictory_response =
        get_typeface_info_detailed(&font_provider, contradictory_request).await?;

    assert_buf_eq!(postscript_name_response, contradictory_response);

    Ok(())
}
