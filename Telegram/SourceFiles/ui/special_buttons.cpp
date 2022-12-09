/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "ui/special_buttons.h"

#include "base/call_delayed.h"
#include "dialogs/ui/dialogs_layout.h"
#include "ui/effects/ripple_animation.h"
#include "ui/effects/radial_animation.h"
#include "ui/image/image_prepare.h"
#include "ui/empty_userpic.h"
#include "ui/ui_utility.h"
#include "data/notify/data_notify_settings.h"
#include "data/data_photo.h"
#include "data/data_session.h"
#include "data/data_folder.h"
#include "data/data_channel.h"
#include "data/data_cloud_file.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "data/data_streaming.h"
#include "data/data_file_origin.h"
#include "history/history.h"
#include "core/file_utilities.h"
#include "core/application.h"
#include "ui/boxes/confirm_box.h"
#include "ui/painter.h"
#include "editor/photo_editor_layer_widget.h"
#include "media/streaming/media_streaming_instance.h"
#include "media/streaming/media_streaming_player.h"
#include "media/streaming/media_streaming_document.h"
#include "settings/settings_calls.h" // Calls::AddCameraSubsection.
#include "calls/calls_instance.h"
#include "webrtc/webrtc_media_devices.h" // Webrtc::GetVideoInputList.
#include "webrtc/webrtc_video_track.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "apiwrap.h"
#include "api/api_peer_photo.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"

namespace Ui {
namespace {

constexpr auto kAnimationDuration = crl::time(120);

bool IsCameraAvailable() {
	return (Core::App().calls().currentCall() == nullptr)
		&& !Webrtc::GetVideoInputList().empty();
}

void CameraBox(
		not_null<Ui::GenericBox*> box,
		not_null<Window::Controller*> controller,
		PeerData *peer,
		Fn<void(QImage &&image)> &&doneCallback) {
	using namespace Webrtc;

	const auto track = Settings::Calls::AddCameraSubsection(
		std::make_shared<Ui::BoxShow>(box),
		box->verticalLayout(),
		false);
	if (!track) {
		box->closeBox();
		return;
	}
	track->stateValue(
	) | rpl::start_with_next([=](const VideoState &state) {
		if (state == VideoState::Inactive) {
			box->closeBox();
		}
	}, box->lifetime());

	auto done = [=, done = std::move(doneCallback)](QImage &&image) {
		box->closeBox();
		done(std::move(image));
	};

	box->setTitle(tr::lng_profile_camera_title());
	box->addButton(tr::lng_continue(), [=, done = std::move(done)]() mutable {
		Editor::PrepareProfilePhoto(
			box,
			controller,
			((peer && peer->isForum())
				? ImageRoundRadius::Large
				: ImageRoundRadius::Ellipse),
			std::move(done),
			track->frame(FrameRequest()).mirrored(true, false));
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

QString CropTitle(not_null<PeerData*> peer) {
	if (peer->isChat() || peer->isMegagroup()) {
		return tr::lng_create_group_crop(tr::now);
	} else if (peer->isChannel()) {
		return tr::lng_create_channel_crop(tr::now);
	} else {
		return tr::lng_settings_crop_profile(tr::now);
	}
}

template <typename Callback>
QPixmap CreateSquarePixmap(int width, Callback &&paintCallback) {
	auto size = QSize(width, width) * cIntRetinaFactor();
	auto image = QImage(size, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(cRetinaFactor());
	image.fill(Qt::transparent);
	{
		Painter p(&image);
		paintCallback(p);
	}
	return Ui::PixmapFromImage(std::move(image));
};

} // namespace

HistoryDownButton::HistoryDownButton(QWidget *parent, const style::TwoIconButton &st) : RippleButton(parent, st.ripple)
, _st(st) {
	resize(_st.width, _st.height);
	setCursor(style::cur_pointer);

	hide();
}

QImage HistoryDownButton::prepareRippleMask() const {
	return Ui::RippleAnimation::EllipseMask(QSize(_st.rippleAreaSize, _st.rippleAreaSize));
}

QPoint HistoryDownButton::prepareRippleStartPosition() const {
	return mapFromGlobal(QCursor::pos()) - _st.rippleAreaPosition;
}

void HistoryDownButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	const auto over = isOver();
	const auto down = isDown();
	((over || down) ? _st.iconBelowOver : _st.iconBelow).paint(p, _st.iconPosition, width());
	paintRipple(p, _st.rippleAreaPosition.x(), _st.rippleAreaPosition.y());
	((over || down) ? _st.iconAboveOver : _st.iconAbove).paint(p, _st.iconPosition, width());
	if (_unreadCount > 0) {
		auto unreadString = QString::number(_unreadCount);

		Dialogs::Ui::UnreadBadgeStyle st;
		st.align = style::al_center;
		st.font = st::historyToDownBadgeFont;
		st.size = st::historyToDownBadgeSize;
		st.sizeId = Dialogs::Ui::UnreadBadgeSize::HistoryToDown;
		Dialogs::Ui::PaintUnreadBadge(p, unreadString, width(), 0, st, 4);
	}
}

void HistoryDownButton::setUnreadCount(int unreadCount) {
	if (_unreadCount != unreadCount) {
		_unreadCount = unreadCount;
		update();
	}
}

UserpicButton::UserpicButton(
	QWidget *parent,
	not_null<Window::Controller*> window,
	not_null<PeerData*> peer,
	Role role,
	const style::UserpicButton &st)
: RippleButton(parent, st.changeButton.ripple)
, _st(st)
, _controller(window->sessionController())
, _window(window)
, _peer(peer)
, _cropTitle(CropTitle(peer))
, _role(role) {
	Expects(_role == Role::ChangePhoto);

	_waiting = false;
	prepare();
}

UserpicButton::UserpicButton(
	QWidget *parent,
	not_null<Window::Controller*> window,
	const QString &cropTitle,
	Role role,
	const style::UserpicButton &st)
: RippleButton(parent, st.changeButton.ripple)
, _st(st)
, _controller(window->sessionController())
, _window(window)
, _cropTitle(cropTitle)
, _role(role) {
	Expects(_role == Role::ChangePhoto || _role == Role::ChoosePhoto);

	_waiting = false;
	prepare();
}

UserpicButton::UserpicButton(
	QWidget *parent,
	not_null<Window::SessionController*> controller,
	not_null<PeerData*> peer,
	Role role,
	const style::UserpicButton &st)
: RippleButton(parent, st.changeButton.ripple)
, _st(st)
, _controller(controller)
, _window(&controller->window())
, _peer(peer)
, _cropTitle(CropTitle(_peer))
, _role(role) {
	processPeerPhoto();
	prepare();
	setupPeerViewers();
}

UserpicButton::UserpicButton(
	QWidget *parent,
	not_null<PeerData*> peer,
	Role role,
	const style::UserpicButton &st)
: RippleButton(parent, st.changeButton.ripple)
, _st(st)
, _peer(peer)
, _cropTitle(CropTitle(_peer))
, _role(role) {
	Expects(_role != Role::OpenProfile && _role != Role::OpenPhoto);

	_waiting = false;
	processPeerPhoto();
	prepare();
	setupPeerViewers();
}

void UserpicButton::prepare() {
	resize(_st.size);
	_notShownYet = _waiting;
	if (!_waiting) {
		prepareUserpicPixmap();
	}
	setClickHandlerByRole();

	if (_role == Role::ChangePhoto) {
		chosenImages(
		) | rpl::start_with_next([=](ChosenImage &&chosen) {
			setImage(std::move(chosen.image));
		}, lifetime());
	}
}

void UserpicButton::setClickHandlerByRole() {
	switch (_role) {
	case Role::ChoosePhoto:
	case Role::ChangePhoto:
		addClickHandler([=] { choosePhotoLocally(); });
		break;

	case Role::OpenPhoto:
		addClickHandler([=] { openPeerPhoto(); });
		break;

	case Role::OpenProfile:
		addClickHandler([this] {
			Expects(_controller != nullptr);

			_controller->showPeerInfo(_peer);
		});
		break;
	}
}

void UserpicButton::changeTo(QImage &&image) {
	setImage(std::move(image));
}

void UserpicButton::choosePhotoLocally() {
	if (!_window) {
		return;
	}
	const auto callback = [=](ChosenType type) {
		return [=](QImage &&image) {
			_chosenImages.fire({ std::move(image), type });
		};
	};
	const auto chooseFile = [=](ChosenType type = ChosenType::Set) {
		base::call_delayed(
			_st.changeButton.ripple.hideDuration,
			crl::guard(this, [=] {
			Editor::PrepareProfilePhotoFromFile(
				this,
				_window,
				((_peer && _peer->isForum())
					? ImageRoundRadius::Large
					: ImageRoundRadius::Ellipse),
				callback(type));
		}));
	};
	if (!IsCameraAvailable()) {
		chooseFile();
	} else {
		_menu = base::make_unique_q<Ui::PopupMenu>(this);
		const auto user = _peer ? _peer->asUser() : nullptr;
		if (user && !user->isSelf()) {
			const auto name = user->firstName.isEmpty()
				? user->name()
				: user->firstName;
			_menu->addAction(
				tr::lng_profile_set_photo_for(tr::now, lt_user, name),
				[=] { chooseFile(); });
			_menu->addAction(
				tr::lng_profile_suggest_photo(tr::now, lt_user, name),
				[=] { chooseFile(ChosenType::Suggest); });
			if (user->flags() & UserDataFlag::PersonalPhoto) {
				_menu->addAction(
					tr::lng_profile_photo_reset(tr::now),
					[=] { user->session().api().peerPhoto().clearPersonal(
						user); _userpicCustom = false; });
			}
		} else {
			_menu->addAction(tr::lng_attach_file(tr::now), [=] {
				chooseFile();
			});
			_menu->addAction(tr::lng_attach_camera(tr::now), [=] {
				_window->show(Box(
					CameraBox,
					_window,
					_peer,
					callback(ChosenType::Set)));
			});
		}
		_menu->popup(QCursor::pos());
	}
}

void UserpicButton::openPeerPhoto() {
	Expects(_peer != nullptr);
	Expects(_controller != nullptr);

	if (_changeOverlayEnabled && _cursorInChangeOverlay) {
		choosePhotoLocally();
		return;
	}

	const auto id = _peer->userpicPhotoId();
	if (!id) {
		return;
	}
	const auto photo = _peer->owner().photo(id);
	if (photo->date && _controller) {
		_controller->openPhoto(photo, _peer);
	}
}

void UserpicButton::setupPeerViewers() {
	_peer->session().changes().peerUpdates(
		_peer,
		Data::PeerUpdate::Flag::Photo
	) | rpl::start_with_next([=] {
		processNewPeerPhoto();
		update();
	}, lifetime());

	_peer->session().downloaderTaskFinished(
	) | rpl::filter([=] {
		return _waiting;
	}) | rpl::start_with_next([=] {
		if (!Ui::PeerUserpicLoading(_userpicView)) {
			_waiting = false;
			startNewPhotoShowing();
		}
	}, lifetime());
}

void UserpicButton::paintEvent(QPaintEvent *e) {
	Painter p(this);

	if (!_waiting && _notShownYet) {
		_notShownYet = false;
		startAnimation();
	}

	auto photoPosition = countPhotoPosition();
	auto photoLeft = photoPosition.x();
	auto photoTop = photoPosition.y();

	if (showSavedMessages()) {
		Ui::EmptyUserpic::PaintSavedMessages(
			p,
			photoPosition.x(),
			photoPosition.y(),
			width(),
			_st.photoSize);
	} else if (showRepliesMessages()) {
		Ui::EmptyUserpic::PaintRepliesMessages(
			p,
			photoPosition.x(),
			photoPosition.y(),
			width(),
			_st.photoSize);
	} else {
		if (_a_appearance.animating()) {
			p.drawPixmapLeft(photoPosition, width(), _oldUserpic);
			p.setOpacity(_a_appearance.value(1.));
		}
		paintUserpicFrame(p, photoPosition);
	}

	const auto fillTranslatedShape = [&](const style::color &color) {
		p.translate(photoLeft, photoTop);
		fillShape(p, color);
		p.translate(-photoLeft, -photoTop);
	};

	if (_role == Role::ChangePhoto || _role == Role::ChoosePhoto) {
		auto over = isOver() || isDown();
		if (over) {
			fillTranslatedShape(_userpicHasImage
				? st::msgDateImgBg
				: _st.changeButton.textBgOver);
		}
		paintRipple(
			p,
			photoLeft,
			photoTop,
			_userpicHasImage
				? &st::shadowFg->c
				: &_st.changeButton.ripple.color->c);
		if (over || !_userpicHasImage) {
			auto iconLeft = (_st.changeIconPosition.x() < 0)
				? (_st.photoSize - _st.changeIcon.width()) / 2
				: _st.changeIconPosition.x();
			auto iconTop = (_st.changeIconPosition.y() < 0)
				? (_st.photoSize - _st.changeIcon.height()) / 2
				: _st.changeIconPosition.y();
			_st.changeIcon.paint(
				p,
				photoLeft + iconLeft,
				photoTop + iconTop,
				width());
		}
	} else if (_changeOverlayEnabled) {
		auto current = _changeOverlayShown.value(
			(isOver() || isDown()) ? 1. : 0.);
		auto barHeight = anim::interpolate(
			0,
			_st.uploadHeight,
			current);
		if (barHeight > 0) {
			auto barLeft = photoLeft;
			auto barTop = photoTop + _st.photoSize - barHeight;
			auto rect = QRect(
				barLeft,
				barTop,
				_st.photoSize,
				barHeight);
			p.setClipRect(rect);
			fillTranslatedShape(_st.uploadBg);
			auto iconLeft = (_st.uploadIconPosition.x() < 0)
				? (_st.photoSize - _st.uploadIcon.width()) / 2
				: _st.uploadIconPosition.x();
			auto iconTop = (_st.uploadIconPosition.y() < 0)
				? (_st.uploadHeight - _st.uploadIcon.height()) / 2
				: _st.uploadIconPosition.y();
			if (iconTop < barHeight) {
				_st.uploadIcon.paint(
					p,
					barLeft + iconLeft,
					barTop + iconTop,
					width());
			}
		}
	}
}

void UserpicButton::paintUserpicFrame(Painter &p, QPoint photoPosition) {
	checkStreamedIsStarted();
	if (_streamed
		&& _streamed->player().ready()
		&& !_streamed->player().videoSize().isEmpty()) {
		const auto paused = _controller
			? _controller->isGifPausedAtLeastFor(
				Window::GifPauseReason::RoundPlaying)
			: false;
		auto request = Media::Streaming::FrameRequest();
		auto size = QSize{ _st.photoSize, _st.photoSize };
		const auto ratio = style::DevicePixelRatio();
		request.outer = request.resize = size * ratio;
		const auto forum = _peer && _peer->isForum();
		if (forum) {
			const auto radius = int(_st.photoSize
				* Ui::ForumUserpicRadiusMultiplier());
			if (_roundingCorners[0].width() != radius * ratio) {
				_roundingCorners = Images::CornersMask(radius);
			}
			request.rounding = Images::CornersMaskRef(_roundingCorners);
		} else {
			if (_ellipseMask.size() != request.outer) {
				_ellipseMask = Images::EllipseMask(size);
			}
			request.mask = _ellipseMask;
		}
		p.drawImage(QRect(photoPosition, size), _streamed->frame(request));
		if (!paused) {
			_streamed->markFrameShown();
		}
	} else {
		p.drawPixmapLeft(photoPosition, width(), _userpic);
	}
}

QPoint UserpicButton::countPhotoPosition() const {
	auto photoLeft = (_st.photoPosition.x() < 0)
		? (width() - _st.photoSize) / 2
		: _st.photoPosition.x();
	auto photoTop = (_st.photoPosition.y() < 0)
		? (height() - _st.photoSize) / 2
		: _st.photoPosition.y();
	return { photoLeft, photoTop };
}

QImage UserpicButton::prepareRippleMask() const {
	return Ui::RippleAnimation::EllipseMask(QSize(
		_st.photoSize,
		_st.photoSize));
}

QPoint UserpicButton::prepareRippleStartPosition() const {
	return (_role == Role::ChangePhoto)
		? mapFromGlobal(QCursor::pos()) - countPhotoPosition()
		: DisabledRippleStartPosition();
}

void UserpicButton::processPeerPhoto() {
	Expects(_peer != nullptr);

	_userpicView = _peer->createUserpicView();
	_waiting = Ui::PeerUserpicLoading(_userpicView);
	if (_waiting) {
		_peer->loadUserpic();
	}
	if (_role == Role::OpenPhoto) {
		if (_peer->userpicPhotoUnknown()) {
			_peer->updateFullForced();
		}
		_canOpenPhoto = (_peer->userpicPhotoId() != 0);
		updateCursor();
		updateVideo();
	}
}

void UserpicButton::updateCursor() {
	Expects(_role == Role::OpenPhoto);

	auto pointer = _canOpenPhoto
		|| (_changeOverlayEnabled && _cursorInChangeOverlay);
	setPointerCursor(pointer);
}

bool UserpicButton::createStreamingObjects(not_null<PhotoData*> photo) {
	Expects(_peer != nullptr);

	using namespace Media::Streaming;

	const auto origin = _peer->isUser()
		? Data::FileOriginUserPhoto(peerToUser(_peer->id), photo->id)
		: Data::FileOrigin(Data::FileOriginPeerPhoto(_peer->id));
	_streamed = std::make_unique<Instance>(
		photo->owner().streaming().sharedDocument(photo, origin),
		nullptr);
	_streamed->lockPlayer();
	_streamed->player().updates(
	) | rpl::start_with_next_error([=](Update &&update) {
		handleStreamingUpdate(std::move(update));
	}, [=](Error &&error) {
		handleStreamingError(std::move(error));
	}, _streamed->lifetime());
	if (_streamed->ready()) {
		streamingReady(base::duplicate(_streamed->info()));
	}
	if (!_streamed->valid()) {
		clearStreaming();
		return false;
	}
	return true;
}

void UserpicButton::clearStreaming() {
	_streamed = nullptr;
	_streamedPhoto = nullptr;
}

void UserpicButton::handleStreamingUpdate(Media::Streaming::Update &&update) {
	using namespace Media::Streaming;

	v::match(update.data, [&](Information &update) {
		streamingReady(std::move(update));
	}, [&](const PreloadedVideo &update) {
	}, [&](const UpdateVideo &update) {
		this->update();
	}, [&](const PreloadedAudio &update) {
	}, [&](const UpdateAudio &update) {
	}, [&](const WaitingForData &update) {
	}, [&](MutedByOther) {
	}, [&](Finished) {
	});
}

void UserpicButton::handleStreamingError(Media::Streaming::Error &&error) {
	Expects(_peer != nullptr);

	_streamedPhoto->setVideoPlaybackFailed();
	_streamedPhoto = nullptr;
	_streamed = nullptr;
}

void UserpicButton::streamingReady(Media::Streaming::Information &&info) {
	update();
}

void UserpicButton::updateVideo() {
	Expects(_role == Role::OpenPhoto);

	const auto id = _peer->userpicPhotoId();
	if (!id) {
		clearStreaming();
		return;
	}
	const auto photo = _peer->owner().photo(id);
	if (!photo->date || !photo->videoCanBePlayed()) {
		clearStreaming();
		return;
	} else if (_streamed && _streamedPhoto == photo) {
		return;
	}
	if (!createStreamingObjects(photo)) {
		photo->setVideoPlaybackFailed();
		return;
	}
	_streamedPhoto = photo;
	checkStreamedIsStarted();
}

void UserpicButton::checkStreamedIsStarted() {
	Expects(!_streamed || _streamedPhoto);

	if (!_streamed) {
		return;
	} else if (_streamed->paused()) {
		_streamed->resume();
	}
	if (_streamed && !_streamed->active() && !_streamed->failed()) {
		const auto position = _streamedPhoto->videoStartPosition();
		auto options = Media::Streaming::PlaybackOptions();
		options.position = position;
		options.mode = Media::Streaming::Mode::Video;
		options.loop = true;
		_streamed->play(options);
	}
}

void UserpicButton::mouseMoveEvent(QMouseEvent *e) {
	RippleButton::mouseMoveEvent(e);
	if (_role == Role::OpenPhoto) {
		updateCursorInChangeOverlay(e->pos());
	}
}

void UserpicButton::updateCursorInChangeOverlay(QPoint localPos) {
	auto photoPosition = countPhotoPosition();
	auto overlayRect = QRect(
		photoPosition.x(),
		photoPosition.y() + _st.photoSize - _st.uploadHeight,
		_st.photoSize,
		_st.uploadHeight);
	auto inOverlay = overlayRect.contains(localPos);
	setCursorInChangeOverlay(inOverlay);
}

void UserpicButton::leaveEventHook(QEvent *e) {
	if (_role == Role::OpenPhoto) {
		setCursorInChangeOverlay(false);
	}
	return RippleButton::leaveEventHook(e);
}

void UserpicButton::setCursorInChangeOverlay(bool inOverlay) {
	Expects(_role == Role::OpenPhoto);

	if (_cursorInChangeOverlay != inOverlay) {
		_cursorInChangeOverlay = inOverlay;
		updateCursor();
	}
}

void UserpicButton::processNewPeerPhoto() {
	if (_userpicCustom) {
		return;
	}
	processPeerPhoto();
	if (!_waiting) {
		grabOldUserpic();
		startNewPhotoShowing();
	}
}

void UserpicButton::grabOldUserpic() {
	auto photoRect = QRect(
		countPhotoPosition(),
		QSize(_st.photoSize, _st.photoSize)
	);
	_oldUserpic = GrabWidget(this, photoRect);
}

void UserpicButton::startNewPhotoShowing() {
	auto oldUniqueKey = _userpicUniqueKey;
	prepareUserpicPixmap();
	update();

	if (_notShownYet) {
		return;
	}
	if (oldUniqueKey != _userpicUniqueKey
		|| _a_appearance.animating()) {
		startAnimation();
	}
}

void UserpicButton::startAnimation() {
	_a_appearance.stop();
	_a_appearance.start([this] { update(); }, 0, 1, _st.duration);
}

void UserpicButton::switchChangePhotoOverlay(
		bool enabled,
		Fn<void(ChosenImage)> chosen) {
	Expects(_role == Role::OpenPhoto);

	if (_changeOverlayEnabled != enabled) {
		_changeOverlayEnabled = enabled;
		if (enabled) {
			if (isOver()) {
				startChangeOverlayAnimation();
			}
			updateCursorInChangeOverlay(
				mapFromGlobal(QCursor::pos()));
			if (chosen) {
				chosenImages() | rpl::start_with_next(chosen, lifetime());
			}
		} else {
			_changeOverlayShown.stop();
			update();
		}
	}
}

void UserpicButton::showSavedMessagesOnSelf(bool enabled) {
	if (_showSavedMessagesOnSelf != enabled) {
		_showSavedMessagesOnSelf = enabled;
		update();
	}
}

bool UserpicButton::showSavedMessages() const {
	return _showSavedMessagesOnSelf && _peer && _peer->isSelf();
}

bool UserpicButton::showRepliesMessages() const {
	return _showSavedMessagesOnSelf && _peer && _peer->isRepliesChat();
}

void UserpicButton::startChangeOverlayAnimation() {
	auto over = isOver() || isDown();
	_changeOverlayShown.start(
		[this] { update(); },
		over ? 0. : 1.,
		over ? 1. : 0.,
		st::slideWrapDuration);
	update();
}

void UserpicButton::onStateChanged(
		State was,
		StateChangeSource source) {
	RippleButton::onStateChanged(was, source);
	if (_changeOverlayEnabled) {
		auto mask = (StateFlag::Over | StateFlag::Down);
		auto wasOver = (was & mask) != 0;
		auto nowOver = (state() & mask) != 0;
		if (wasOver != nowOver) {
			startChangeOverlayAnimation();
		}
	}
}

void UserpicButton::setImage(QImage &&image) {
	grabOldUserpic();

	auto size = QSize(_st.photoSize, _st.photoSize);
	auto small = image.scaled(
		size * cIntRetinaFactor(),
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	const auto forum = _peer && _peer->isForum();
	_userpic = Ui::PixmapFromImage(forum
		? Images::Round(std::move(small), Images::Option::RoundLarge)
		: Images::Circle(std::move(small)));
	_userpic.setDevicePixelRatio(cRetinaFactor());
	_userpicCustom = _userpicHasImage = true;
	_userpicUniqueKey = {};
	_result = std::move(image);

	startNewPhotoShowing();
}

void UserpicButton::fillShape(QPainter &p, const style::color &color) const {
	PainterHighQualityEnabler hq(p);
	p.setPen(Qt::NoPen);
	p.setBrush(color);
	const auto size = _st.photoSize;
	if (_peer && _peer->isForum()) {
		const auto radius = size * Ui::ForumUserpicRadiusMultiplier();
		p.drawRoundedRect(0, 0, size, size, radius, radius);
	} else {
		p.drawEllipse(0, 0, size, size);
	}
}

void UserpicButton::prepareUserpicPixmap() {
	if (_userpicCustom) {
		return;
	}
	auto size = _st.photoSize;
	_userpicHasImage = _peer
		&& (_peer->userpicCloudImage(_userpicView)
			|| _role != Role::ChangePhoto);
	_userpic = CreateSquarePixmap(size, [&](Painter &p) {
		if (_userpicHasImage) {
			_peer->paintUserpic(p, _userpicView, 0, 0, _st.photoSize);
		} else {
			fillShape(p, _st.changeButton.textBg);
		}
	});
	_userpicUniqueKey = _userpicHasImage
		? _peer->userpicUniqueKey(_userpicView)
		: InMemoryKey();
}

SilentToggle::SilentToggle(QWidget *parent, not_null<ChannelData*> channel)
: RippleButton(parent, st::historySilentToggle.ripple)
, _st(st::historySilentToggle)
, _channel(channel)
, _checked(channel->owner().notifySettings().silentPosts(_channel)) {
	Expects(!channel->owner().notifySettings().silentPostsUnknown(_channel));

	resize(_st.width, _st.height);

	paintRequest(
	) | rpl::start_with_next([=](const QRect &clip) {
		auto p = QPainter(this);
		paintRipple(p, _st.rippleAreaPosition, nullptr);

		//const auto checked = _crossLineAnimation.value(_checked ? 1. : 0.);
		const auto over = isOver();
		(_checked
			? (over
				? st::historySilentToggleOnOver
				: st::historySilentToggleOn)
			: (over
				? st::historySilentToggle.iconOver
				: st::historySilentToggle.icon)).paintInCenter(p, rect());
	}, lifetime());

	setMouseTracking(true);
}

void SilentToggle::mouseMoveEvent(QMouseEvent *e) {
	RippleButton::mouseMoveEvent(e);
	if (rect().contains(e->pos())) {
		Ui::Tooltip::Show(1000, this);
	} else {
		Ui::Tooltip::Hide();
	}
}

void SilentToggle::setChecked(bool checked) {
	if (_checked != checked) {
		_checked = checked;
		_crossLineAnimation.start(
			[=] { update(); },
			_checked ? 0. : 1.,
			_checked ? 1. : 0.,
			kAnimationDuration);
	}
}

void SilentToggle::leaveEventHook(QEvent *e) {
	RippleButton::leaveEventHook(e);
	Ui::Tooltip::Hide();
}

void SilentToggle::mouseReleaseEvent(QMouseEvent *e) {
	setChecked(!_checked);
	RippleButton::mouseReleaseEvent(e);
	Ui::Tooltip::Show(0, this);
	_channel->owner().notifySettings().update(_channel, {}, _checked);
}

QString SilentToggle::tooltipText() const {
	return _checked
		? tr::lng_wont_be_notified(tr::now)
		: tr::lng_will_be_notified(tr::now);
}

QPoint SilentToggle::tooltipPos() const {
	return QCursor::pos();
}

bool SilentToggle::tooltipWindowActive() const {
	return Ui::AppInFocus() && InFocusChain(window());
}

QPoint SilentToggle::prepareRippleStartPosition() const {
	const auto result = mapFromGlobal(QCursor::pos())
		- _st.rippleAreaPosition;
	const auto rect = QRect(0, 0, _st.rippleAreaSize, _st.rippleAreaSize);
	return rect.contains(result)
		? result
		: DisabledRippleStartPosition();
}

QImage SilentToggle::prepareRippleMask() const {
	return RippleAnimation::EllipseMask(
		QSize(_st.rippleAreaSize, _st.rippleAreaSize));
}

} // namespace Ui
