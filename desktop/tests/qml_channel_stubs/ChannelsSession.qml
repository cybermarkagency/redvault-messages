// Stand-in for ChannelsSession: what the channel page reads, and a record of what it asks for.
// `posts` is set to the real ChannelPostModel by the test.
import QtQml

QtObject {
    property string channelId: "c-1"
    property var channel: ({ id: "c-1", name: "Night Market", description: "Stalls, hours and maps.", isPrivate: false, hasPhoto: false,
                             photo: "", link: "https://example.test/?channel=c-1", acceptSuggestions: true })
    property var posts: null
    property bool hasOlder: false
    property bool isMine: false
    property bool isFollowing: false
    property bool isMuted: false
    property bool isAdmin: false
    property bool canManage: isMine || isAdmin
    property bool busy: false
    property bool posting: false
    property string error: ""
    property string notice: ""
    property string composerError: ""
    property var scheduled: []
    property var suggestions: []
    property var mySuggestions: []
    property bool suggestionSent: false
    property string suggestionError: ""
    property ChannelUploads postUploads: ChannelUploads {}
    property ChannelUploads suggestionUploads: ChannelUploads {}
    readonly property int maxPostLength: 4096
    readonly property int maxNameLength: 80
    readonly property int maxDescriptionLength: 1000
    readonly property int maxPollOptions: 10
    readonly property int maxChecklistItems: 30

    signal posted()
    signal detailsSaved()
    signal suggestionDecided()

    // The record.
    property string lastBody: ""
    property var lastExtra: null
    property string lastPublishAt: ""
    property string lastDeletedPost: ""
    property string lastDeletedScheduled: ""
    property string lastPublishedScheduled: ""
    property var lastVote: null
    property int olderLoads: 0
    property int follows: 0

    function formatDuration(seconds) { return "0:00" }
    function loadOlder() { ++olderLoads }
    function toggleFollow() { ++follows; isFollowing = !isFollowing }
    function toggleMuted() { isMuted = !isMuted }
    function submitPost(body, extra, publishAt) { lastBody = body; lastExtra = extra; lastPublishAt = publishAt; posted() }
    function deletePost(postId) { lastDeletedPost = postId }
    function deleteScheduled(postId) { lastDeletedScheduled = postId }
    function publishScheduled(postId) { lastPublishedScheduled = postId }
    function vote(postId, options) { lastVote = { postId: postId, options: options } }
    function closePoll(postId) {}
    function tick(postId, index) {}
    function clearMessages() { error = ""; notice = "" }
    function setNotice(text) { notice = text }
    function isOwnChannelLink(link) { return false }
    function openChannelLink(link) {}
    function resetSuggestionForm() {}
    function sendSuggestion(body) {}
    function report(reason) {}
    function copyLink() {}
    function saveMedia(url, file) {}
}
